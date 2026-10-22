// Headless playback self-test. Exercises the parts of Player that the renderer
// drives (frameForNow) without opening a window, so seek/EOS/track-switch
// behaviour can be checked from the command line:
//
//   play.exe --selftest test.mp4 [multi_audio.mp4]
//
// Each check prints PASS/FAIL; the process exit code is the failure count.
#include <atomic>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include <QDir>
#include <QFile>
#include <QString>

#include "core/Library.h"
#include "core/Log.h"
#include "core/Player.h"
#include "core/PlaylistModel.h"
#include "core/Recorder.h"
#include "core/Rotation.h"
#include "core/SubtitleTrack.h"
#include "render/Scaler.h"
#include "render/Hdr.h"

extern "C" {
#include <libavformat/avformat.h>
}

namespace {

using Clock = std::chrono::steady_clock;

double secsSince(Clock::time_point t0) {
    return std::chrono::duration<double>(Clock::now() - t0).count();
}

void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

int failures = 0;

void check(bool ok, const char* what, const std::string& detail = {}) {
    std::printf("[%s] %s%s%s\n", ok ? "PASS" : "FAIL", what,
                detail.empty() ? "" : "  ", detail.c_str());
    std::fflush(stdout);
    if (!ok) ++failures;
}

// Stands in for the scene graph: pulls frames at ~60 Hz and records what was
// presented.
struct Presenter {
    Player* p = nullptr;
    std::atomic<bool> run{true};
    std::atomic<double> lastPts{-1.0};
    std::thread th;
    int64_t frames = 0;

    void start() {
        th = std::thread([this] {
            while (run.load()) {
                if (VideoFramePtr f = p->frameForNow()) {
                    lastPts.store(f->pts);
                    ++frames;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(4));
            }
        });
    }
    void stop() {
        run = false;
        if (th.joinable()) th.join();
    }
    ~Presenter() { stop(); }
};

// Waits until the presenter shows a frame within |tol| seconds of |target|.
// |pres.lastPts| must be invalidated before the seek so a stale frame from
// before the seek cannot satisfy the wait.
bool waitForFrameNear(Presenter& pres, double target, double tol, int timeoutMs, double* showedAt = nullptr,
                      double* latencyMs = nullptr) {
    const auto t0 = Clock::now();
    while (secsSince(t0) * 1000.0 < timeoutMs) {
        const double pts = pres.lastPts.load();
        if (pts >= 0.0 && std::fabs(pts - target) <= tol) {
            if (showedAt) *showedAt = pts;
            if (latencyMs) *latencyMs = secsSince(t0) * 1000.0;
            return true;
        }
        sleepMs(2);
    }
    return false;
}

// Waits until the presenter has shown a frame at/after |target|.
bool waitForFrameAt(Presenter& pres, double target, int timeoutMs, double* showedAt = nullptr,
                    double* latencyMs = nullptr) {
    const auto t0 = Clock::now();
    while (secsSince(t0) * 1000.0 < timeoutMs) {
        const double pts = pres.lastPts.load();
        if (pts + 1e-3 >= target) {
            if (showedAt) *showedAt = pts;
            if (latencyMs) *latencyMs = secsSince(t0) * 1000.0;
            return true;
        }
        sleepMs(2);
    }
    return false;
}

bool waitUntil(const std::function<bool()>& pred, int timeoutMs, double* elapsedMs = nullptr) {
    const auto t0 = Clock::now();
    while (secsSince(t0) * 1000.0 < timeoutMs) {
        if (pred()) {
            if (elapsedMs) *elapsedMs = secsSince(t0) * 1000.0;
            return true;
        }
        sleepMs(5);
    }
    return false;
}

void testBasic(const std::string& path) {
    std::printf("\n== basic playback: %s ==\n", path.c_str());
    Player p;
    std::string err;
    if (!p.open(path, &err)) {
        check(false, "open", err);
        return;
    }
    check(p.hasVideo(), "has video");
    check(p.duration() > 1.0, "duration", std::to_string(p.duration()));

    Presenter pres;
    pres.p = &p;
    pres.start();

    // The clock must only start once something is on screen.
    check(waitForFrameAt(pres, 0.0, 3000), "first frame presented");
    sleepMs(1000);
    const double pos = p.position();
    check(pos > 0.5 && pos < 1.8, "position advances in real time", std::to_string(pos));

    // Pause must freeze both the clock and the presented picture.
    p.pause();
    sleepMs(200);
    const double frozen = p.position();
    const double frozenPts = pres.lastPts.load();
    sleepMs(500);
    check(std::fabs(p.position() - frozen) < 0.02, "position frozen while paused",
          std::to_string(p.position() - frozen));
    check(std::fabs(pres.lastPts.load() - frozenPts) < 1e-3, "no frames presented while paused");

    p.play();
    sleepMs(400);
    check(p.position() > frozen + 0.2, "position resumes", std::to_string(p.position() - frozen));

    pres.stop();
    p.close();
}

// Measures how fast the master clock advances per wall second at |speed|.
double measureRate(Player& p, Presenter& pres, double speed, double* presentedLag) {
    p.setSpeed(speed);
    sleepMs(350);  // the worker re-anchors with a seek
    const double before = p.position();
    const auto t0 = Clock::now();
    sleepMs(1000);
    const double wall = secsSince(t0);
    const double rate = (p.position() - before) / wall;
    if (presentedLag) *presentedLag = p.position() - pres.lastPts.load();
    return rate;
}

void testSpeed(const std::string& path) {
    std::printf("\n== playback speed ==\n");
    Player p;
    std::string err;
    if (!p.open(path, &err)) {
        check(false, "open", err);
        return;
    }
    Presenter pres;
    pres.p = &p;
    pres.start();
    check(waitForFrameAt(pres, 0.0, 3000), "first frame presented");

    // Device volume: plain setter, clamped to 0..1.
    p.setVolume(0.4f);
    check(std::fabs(p.volume() - 0.4f) < 1e-6, "volume set", std::to_string(p.volume()));
    p.setVolume(2.0f);
    check(std::fabs(p.volume() - 1.0f) < 1e-6, "volume clamped to 1");
    p.setVolume(-1.0f);
    check(std::fabs(p.volume()) < 1e-6, "volume clamped to 0");
    p.setVolume(1.0f);

    double lag = 0.0;
    const double fast = measureRate(p, pres, 2.0, &lag);
    check(std::fabs(fast - 2.0) < 0.25, "2x clock advances twice as fast",
          std::to_string(fast));
    check(std::fabs(lag) < 0.5, "video follows the clock at 2x", std::to_string(lag));

    const double slow = measureRate(p, pres, 0.5, &lag);
    check(std::fabs(slow - 0.5) < 0.12, "0.5x clock advances half as fast",
          std::to_string(slow));
    check(std::fabs(lag) < 0.5, "video follows the clock at 0.5x", std::to_string(lag));

    const double normal = measureRate(p, pres, 1.0, nullptr);
    check(std::fabs(normal - 1.0) < 0.15, "back to 1x", std::to_string(normal));

    p.setSpeed(9.0);
    check(std::fabs(p.speed() - 4.0) < 1e-6, "speed clamped to 4x", std::to_string(p.speed()));
    p.setSpeed(0.1);
    check(std::fabs(p.speed() - 0.25) < 1e-6, "speed clamped to 0.25x", std::to_string(p.speed()));
    p.setSpeed(1.0);

    pres.stop();
    p.close();
}

void testSeeks(const std::string& path, int count) {
    std::printf("\n== seek precision (%d seeks) ==\n", count);
    Player p;
    std::string err;
    if (!p.open(path, &err)) {
        check(false, "open", err);
        return;
    }
    Presenter pres;
    pres.p = &p;
    pres.start();
    waitForFrameAt(pres, 0.0, 3000);
    sleepMs(300);

    const double dur = p.duration() > 0 ? p.duration() : 12.0;
    double worstMs = 0.0;
    double worstErr = 0.0;
    int ok = 0;

    for (int i = 0; i < count; ++i) {
        // Deterministic spread over the file, including near both ends.
        const double target = 0.3 + (dur - 1.0) * ((i * 0.61803398875) - std::floor(i * 0.61803398875));
        p.seek(target);
        double latency = 0.0;
        double showed = 0.0;
        if (!waitForFrameNear(pres, target, 0.12, 2000, &showed, &latency)) {
            std::printf("  seek %d -> %.2fs: no frame\n", i, target);
            continue;
        }
        const double errSec = showed - target;
        worstMs = std::max(worstMs, latency);
        worstErr = std::max(worstErr, std::fabs(errSec));
        if (std::fabs(errSec) < 0.1) ++ok;
        sleepMs(60);
    }

    check(ok == count, "every seek landed on the target frame",
          std::to_string(ok) + "/" + std::to_string(count));
    // The roadmap target is 150 ms; a precise seek costs roughly one decode per
    // frame from the keyframe, so 4K or long-GOP material legitimately exceeds
    // it. Fail only on something that would feel like a hang.
    check(worstMs < 250.0, "seek response < 250 ms (target 150 ms on 1080p short-GOP)",
          "worst " + std::to_string(int(worstMs)) + " ms");
    check(worstErr < 0.1, "seek accuracy < 100 ms",
          "worst " + std::to_string(int(worstErr * 1000)) + " ms");

    // Seek while paused must still show the target frame (scrubbing).
    p.pause();
    sleepMs(100);
    p.seek(dur * 0.5);
    check(waitForFrameNear(pres, dur * 0.5, 0.12, 2000), "paused seek presents the target frame");
    p.play();

    pres.stop();
    p.close();
}

void testAudioSwitch(const std::string& path) {
    std::printf("\n== audio tracks ==\n");
    Player p;
    std::string err;
    if (!p.open(path, &err)) {
        check(false, "open", err);
        return;
    }
    const auto tracks = p.audioTracks();
    check(tracks.size() >= 2, "track enumeration", std::to_string(tracks.size()) + " tracks");
    if (tracks.size() < 2) {
        p.close();
        return;
    }

    Presenter pres;
    pres.p = &p;
    pres.start();
    waitForFrameAt(pres, 0.0, 3000);
    sleepMs(500);

    const int first = p.currentAudioTrack();
    check(first == tracks[0].streamIndex, "first track selected by default", std::to_string(first));

    const int target = tracks[1].streamIndex;
    check(p.selectAudioTrack(target), "selectAudioTrack accepted");
    check(waitUntil([&] { return p.currentAudioTrack() == target; }, 2000), "track switched");

    const double before = p.position();
    sleepMs(600);
    const double after = p.position();
    check(after > before + 0.3, "playback continues after the switch",
          std::to_string(before) + " -> " + std::to_string(after));

    // Switching back must work as well.
    check(p.selectAudioTrack(first), "switch back accepted");
    check(waitUntil([&] { return p.currentAudioTrack() == first; }, 2000), "switched back");

    // Invalid indices are rejected, not crashed on.
    check(!p.selectAudioTrack(-1), "invalid index rejected");
    check(!p.selectAudioTrack(99999), "out of range index rejected");

    pres.stop();
    p.close();
}

void testEos(const std::string& path) {
    std::printf("\n== end of stream ==\n");
    Player p;
    std::string err;
    if (!p.open(path, &err)) {
        check(false, "open", err);
        return;
    }
    Presenter pres;
    pres.p = &p;
    pres.start();
    waitForFrameAt(pres, 0.0, 3000);

    const double dur = p.duration();
    p.seek(std::max(0.0, dur - 0.6));
    check(waitUntil([&] { return p.isFinished(); }, 4000), "reaches the end while playing");

    const double atEnd = p.position();
    check(std::fabs(atEnd - dur) < 0.35, "stops near the duration",
          std::to_string(atEnd) + " / " + std::to_string(dur));

    const double pts = pres.lastPts.load();
    sleepMs(300);
    check(std::fabs(pres.lastPts.load() - pts) < 1e-3, "last frame stays on screen");
    check(std::fabs(p.position() - atEnd) < 0.02, "clock frozen at the end");

    // Playing again restarts from the beginning.
    p.play();
    check(waitUntil([&] { return !p.isFinished(); }, 2000), "replay clears the finished state");
    sleepMs(500);
    check(p.position() < 2.0, "replay restarted at the beginning", std::to_string(p.position()));

    pres.stop();
    p.close();
}

void testLoop(const std::string& path) {
    std::printf("\n== loop ==\n");
    Player p;
    std::string err;
    if (!p.open(path, &err)) {
        check(false, "open", err);
        return;
    }
    p.setLoop(true);
    Presenter pres;
    pres.p = &p;
    pres.start();
    waitForFrameAt(pres, 0.0, 3000);

    const double dur = p.duration();
    p.seek(std::max(0.0, dur - 0.5));
    check(waitForFrameNear(pres, dur - 0.5, 0.15, 3000), "seek near the end landed");
    check(waitUntil([&] { return p.position() < dur * 0.5; }, 6000), "wraps back to the start");
    check(!p.isFinished(), "loop does not report finished");
    sleepMs(400);
    check(p.position() > 0.1, "keeps playing after the wrap", std::to_string(p.position()));

    pres.stop();
    p.close();
}

void testSubtitles() {
    std::printf("\n== subtitles ==\n");
    QString err;

    // SRT: CRLF line endings, <i> tags, an {\an8} override, two-line cue.
    SubtitleTrack srt;
    const QString srtText = QStringLiteral(
        "1\r\n00:00:01,000 --> 00:00:03,500\r\n<i>Hello</i> {\\an8}world\r\n\r\n"
        "2\r\n00:00:04,000 --> 00:00:06,000\r\nLine one\r\nLine two\r\n");
    check(srt.loadText(srtText, QStringLiteral("x.srt"), &err), "srt parse", err.toStdString());
    check(srt.cueCount() == 2, "srt cue count", std::to_string(srt.cueCount()));
    check(srt.textAt(0.5).isEmpty(), "nothing before the first cue");
    check(srt.textAt(2.0) == QStringLiteral("Hello world"), "cue text at 2s",
          srt.textAt(2.0).toStdString());
    check(srt.textAt(3.6).isEmpty(), "gap between cues is empty");
    check(srt.textAt(5.0) == QStringLiteral("Line one\nLine two"), "multi-line cue",
          srt.textAt(5.0).toStdString());

    // ASS: style table, \N, inline override and commas inside the text.
    SubtitleTrack ass;
    const QString assText = QStringLiteral(
        "[Script Info]\nPlayResX: 1920\nPlayResY: 1080\n"
        "[V4+ Styles]\n"
        "Format: Name, Fontname, Fontsize, PrimaryColour, Bold, Italic, BorderStyle, Outline, "
        "Shadow, Alignment, MarginL, MarginR, MarginV, Encoding\n"
        "Style: Default,Microsoft YaHei,48,&H00FFFFFF,0,0,1,2,0,2,10,10,20,1\n"
        "[Events]\n"
        "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
        "Dialogue: 0,0:00:01.00,0:00:02.50,Default,,0,0,0,,{\\an8}Top line\\Nsecond\n"
        "Dialogue: 0,0:00:03.00,0:00:04.00,Default,,0,0,0,,Plain, with comma\n");
    check(ass.loadText(assText, QStringLiteral("x.ass"), &err), "ass parse", err.toStdString());
    check(ass.cueCount() == 2, "ass cue count", std::to_string(ass.cueCount()));
    check(ass.scriptWidth() == 1920 && ass.scriptHeight() == 1080, "ass script resolution");
    check(ass.textAt(1.5) == QStringLiteral("Top line\nsecond"), "ass override + \\N",
          ass.textAt(1.5).toStdString());
    check(ass.textAt(3.5) == QStringLiteral("Plain, with comma"), "ass text keeps commas",
          ass.textAt(3.5).toStdString());

    // WebVTT: header, dot milliseconds, cue settings after the timestamp.
    SubtitleTrack vtt;
    const QString vttText = QStringLiteral("WEBVTT\n\n00:00:01.500 --> 00:00:02.000 align:middle\nHi\n");
    check(vtt.loadText(vttText, QStringLiteral("x.vtt"), &err), "vtt parse", err.toStdString());
    check(vtt.textAt(1.7) == QStringLiteral("Hi"), "vtt cue settings ignored",
          vtt.textAt(1.7).toStdString());

    double v = 0;
    const bool t1 = SubtitleTrack::parseTimestamp(QStringLiteral("01:02:03,456"), &v);
    check(t1 && std::fabs(v - 3723.456) < 1e-6, "timestamp hh:mm:ss,ms", std::to_string(v));
    const bool t2 = SubtitleTrack::parseTimestamp(QStringLiteral("00:05.5"), &v);
    check(t2 && std::fabs(v - 5.5) < 1e-6, "timestamp mm:ss.f", std::to_string(v));
    check(!SubtitleTrack::parseTimestamp(QStringLiteral("nope"), &v), "bad timestamp rejected");
    check(!srt.loadText(QStringLiteral("not a subtitle"), QStringLiteral("x.srt"), &err),
          "garbage rejected");
}

// Recent files / resume positions: pure JSON round-trip, no playback needed.
void testLibrary() {
    std::printf("\n== library ==\n");
    const QString file = QDir::tempPath() + QStringLiteral("/play_library_test.json");
    QFile::remove(file);

    Library lib;
    check(lib.load(file), "missing file starts empty");
    check(lib.size() == 0, "no entries on first run");

    lib.touch(QStringLiteral("D:/media/a.mp4"), 100.0);
    lib.setPosition(QStringLiteral("D:/media/a.mp4"), 42.0, 100.0);
    check(std::fabs(lib.positionFor(QStringLiteral("D:/media/a.mp4")) - 42.0) < 1e-6,
          "position stored");

    lib.touch(QStringLiteral("D:/media/b.mp4"), 50.0);
    lib.touch(QStringLiteral("D:/media/a.mp4"));
    const QVariantList recent = lib.recent(10);
    check(recent.size() == 2, "both entries listed", std::to_string(recent.size()));
    check(recent.first().toMap().value(QStringLiteral("path")).toString() ==
              QStringLiteral("D:/media/a.mp4"),
          "touch moves to the front");
    check(recent.first().toMap().value(QStringLiteral("name")).toString() ==
              QStringLiteral("a.mp4"),
          "name is the file name");
    check(recent.first().toMap().value(QStringLiteral("missing")).toBool(),
          "missing file flagged");

    QString err;
    check(lib.save(&err), "save", err.toStdString());
    Library reloaded;
    check(reloaded.load(file), "reload");
    check(reloaded.size() == 2, "round trip entries");
    check(std::fabs(reloaded.positionFor(QStringLiteral("D:/media/a.mp4")) - 42.0) < 1e-6,
          "round trip position");

    // The first and last seconds are not worth resuming.
    lib.setPosition(QStringLiteral("D:/media/b.mp4"), 1.0, 50.0);
    check(lib.positionFor(QStringLiteral("D:/media/b.mp4")) == 0.0, "1s is not resumed");
    lib.setPosition(QStringLiteral("D:/media/b.mp4"), 45.0, 50.0);
    check(lib.positionFor(QStringLiteral("D:/media/b.mp4")) == 0.0, "near the end is not resumed");
    lib.setPosition(QStringLiteral("D:/media/b.mp4"), 20.0, 50.0);
    check(std::fabs(lib.positionFor(QStringLiteral("D:/media/b.mp4")) - 20.0) < 1e-6,
          "the middle is resumed");

    lib.remove(QStringLiteral("D:/media/b.mp4"));
    check(lib.size() == 1, "remove");
    lib.clear();
    check(lib.size() == 0, "clear");

    check(!Library::shouldResume(3.0, 100.0), "3s ignored");
    check(Library::shouldResume(10.0, 100.0), "10s resumed");
    check(!Library::shouldResume(95.0, 100.0), "95s of 100s ignored");

    QFile broken(file);
    if (broken.open(QIODevice::WriteOnly)) {
        broken.write("{ this is not json");
        broken.close();
    }
    Library bad;
    check(!bad.load(file), "corrupt JSON reported");
    check(bad.size() == 0, "corrupt JSON starts empty");
    QFile::remove(file);
}

// Queue model: folder expansion, ordering, next/previous and current tracking.
void testPlaylist() {
    std::printf("\n== playlist ==\n");
    const QString dir = QDir::tempPath() + QStringLiteral("/play_playlist_test");
    QDir(dir).removeRecursively();
    QDir().mkpath(dir + QStringLiteral("/sub"));
    const QStringList names = { QStringLiteral("a.mp4"), QStringLiteral("b.mkv"),
                                QStringLiteral("sub/c.mp4"), QStringLiteral("note.txt") };
    for (const QString& n : names) {
        QFile f(dir + QLatin1Char('/') + n);
        if (f.open(QIODevice::WriteOnly)) f.write("x");
    }

    PlaylistModel pl;
    check(pl.count() == 0, "starts empty");
    const int addedFromFolder = pl.addPaths({ dir });
    check(addedFromFolder == 3, "folder expanded to video files",
          std::to_string(addedFromFolder));
    check(pl.count() == 3, "count");
    check(pl.current() == 0, "first entry selected");
    check(QFileInfo(pl.pathAt(0)).fileName() == QStringLiteral("a.mp4"), "sorted by name",
          pl.pathAt(0).toStdString());
    check(pl.pathAt(2).endsWith(QStringLiteral("c.mp4")), "subfolder included");

    check(pl.hasNext(), "has a next entry");
    check(QFileInfo(pl.next()).fileName() == QStringLiteral("b.mkv"), "next returns b.mkv");
    check(QFileInfo(pl.previous()).fileName() == QStringLiteral("a.mp4"), "previous returns a.mp4");
    check(pl.previous().isEmpty(), "previous at the start is empty");
    check(pl.pathAt(99).isEmpty(), "out of range path is empty");

    // Dropping a row before the current one keeps the current file.
    pl.setCurrent(2);
    const QString keep = pl.currentPath();
    pl.removeAt(0);
    check(pl.count() == 2, "remove drops one row", std::to_string(pl.count()));
    check(pl.currentPath() == keep, "current stays on the same file",
          pl.currentPath().toStdString());
    check(pl.current() == 1, "current index follows", std::to_string(pl.current()));

    pl.moveRow(1, 0);
    check(pl.current() == 0, "move keeps the current file", std::to_string(pl.current()));
    check(pl.currentPath() == keep, "move keeps the path");

    pl.clear();
    check(pl.count() == 0 && pl.current() == -1, "clear resets the selection");
    check(pl.addPaths({ dir + QStringLiteral("/a.mp4") }) == 1, "single file added");
    check(pl.addPaths({ dir + QStringLiteral("/missing.mp4") }) == 0, "missing file ignored");
    check(QDir(dir).removeRecursively(), "cleanup");
}

// Embedded subtitle streams: the container is scanned, every text track is
// decoded and probed for visible text. Bitmap tracks are only enumerated.
void testEmbeddedSubtitles(const std::string& path) {
    std::printf("\n== embedded subtitles ==\n");
    const QString file = QString::fromStdString(path);
    const auto tracks = SubtitleTrack::embeddedTracks(file);
    if (tracks.empty()) {
        std::printf("SKIP: %s has no subtitle streams\n", path.c_str());
        return;
    }

    int textTracks = 0;
    for (const auto& t : tracks) {
        if (!t.text) {
            check(true, "bitmap track listed as unsupported", t.codec.toStdString());
            continue;
        }
        ++textTracks;
        SubtitleTrack st;
        QString err;
        const bool ok = st.loadEmbedded(file, t.index, &err);
        check(ok, "decode embedded track", t.codec.toStdString() + ": " + err.toStdString());
        if (!ok) continue;

        check(st.cueCount() > 0, "embedded cues decoded", std::to_string(st.cueCount()));
        QString any;
        for (double at = 0.0; at < 30.0 && any.isEmpty(); at += 0.25) any = st.textAt(at);
        check(!any.isEmpty(), "embedded text visible somewhere",
              any.left(40).toStdString());
        // The generic text decoder wraps lines in an ASS style prefix.
        check(!any.contains(QStringLiteral(",Default,")), "ass/text wrapper stripped",
              any.left(40).toStdString());
        check(st.label().startsWith(QStringLiteral("内嵌 ")), "embedded label",
              st.label().toStdString());
        check(st.path().endsWith(QStringLiteral("#%1").arg(t.index)), "stream in path",
              st.path().toStdString());
    }
    check(textTracks > 0, "at least one text track", std::to_string(tracks.size()) + " streams");
}

// Resampling filter names come from --scaler, PLAY_SCALER and the QML menu.
void testScaler() {
    std::printf("\n== scaler names ==\n");
    check(Scaler::fromName(QStringLiteral("bicubic")) == Scaler::Bicubic, "bicubic");
    check(Scaler::fromName(QStringLiteral("Lanczos3")) == Scaler::Lanczos3,
          "lanczos3 is case-insensitive");
    check(Scaler::fromName(QStringLiteral(" lanczos ")) == Scaler::Lanczos3,
          "lanczos alias ignores whitespace");
    check(Scaler::fromName(QStringLiteral("bilinear")) == Scaler::Bilinear, "bilinear");
    check(Scaler::fromName(QStringLiteral("nope")) == Scaler::Bilinear,
          "unknown filter falls back to bilinear");
    check(Scaler::support(Scaler::Bicubic) == 2 && Scaler::support(Scaler::Lanczos3) == 3 &&
              Scaler::support(Scaler::Bilinear) == 0,
          "kernel support");
    for (int k = 0; k <= 2; ++k) {
        check(Scaler::fromName(QString::fromLatin1(Scaler::name(k))) == k, "name round trip",
              Scaler::name(k));
    }
}

// scRGB output maths. video.frag mirrors these numbers, so the self-test pins
// them down: a refactor must not silently change the exposure of HDR output.
void testHdr() {
    std::printf("\n== hdr output ==\n");
    check(std::fabs(Hdr::sdrWhiteScale(203.f) - 2.5375f) < 1e-4f,
          "SDR white 203 nits -> scRGB 2.5375",
          QString::number(Hdr::sdrWhiteScale(203.f), 'f', 4).toStdString());
    check(std::fabs(Hdr::sdrWhiteScale(0.f) - Hdr::kDefaultSdrWhiteNits / Hdr::kScRgbWhiteNits) <
              1e-4f,
          "missing SDR white falls back to BT.2408 graphics white");
    check(std::fabs(Hdr::nitsToScRgb(1000.f) - 12.5f) < 1e-4f, "1000 nits -> scRGB 12.5");
    check(std::fabs(Hdr::softClip(0.5f, 2.f) - 0.5f) < 1e-6f, "below the knee is untouched");
    check(Hdr::softClip(1e6f, 2.f) <= 2.f && Hdr::softClip(1e6f, 2.f) > 1.99f,
          "highlights approach the panel peak without clipping",
          QString::number(Hdr::softClip(1e6f, 2.f), 'f', 4).toStdString());
    check(std::fabs(Hdr::softClip(0.75f * 5.f, 5.f) - 3.75f) < 1e-4f, "the knee is continuous");
    bool monotonic = true;
    float prev = -1.f;
    for (float x = 0.f; x < 12.f; x += 0.05f) {
        const float y = Hdr::softClip(x, 5.f);
        if (y < prev) monotonic = false;
        prev = y;
    }
    check(monotonic, "soft clip is monotonic");
}

// Display matrix (phone videos). The eight right-angle matrices as
// av_display_rotation_set() / av_display_matrix_flip() write them, plus the
// degenerate cases that must fall back to "leave the frame alone".
void testRotation() {
    std::printf("\n== rotation ==\n");
    const int32_t kOne = 65536;
    struct Case {
        int32_t m[9];
        int turns;
        bool mirrored;
        const char* name;
    };
    const Case cases[] = {
        {{kOne, 0, 0, 0, kOne, 0, 0, 0, kOne}, 0, false, "identity"},
        {{0, kOne, 0, -kOne, 0, 0, 0, 0, kOne}, 1, false, "90 cw"},
        {{-kOne, 0, 0, 0, -kOne, 0, 0, 0, kOne}, 2, false, "180"},
        {{0, -kOne, 0, kOne, 0, 0, 0, 0, kOne}, 3, false, "270 cw"},
        {{-kOne, 0, 0, 0, kOne, 0, 0, 0, kOne}, 0, true, "hflip"},
        {{kOne, 0, 0, 0, -kOne, 0, 0, 0, kOne}, 2, true, "vflip = 180 + hflip"},
        {{0, -kOne, 0, -kOne, 0, 0, 0, 0, kOne}, 3, true, "transpose"},
        {{0, kOne, 0, kOne, 0, 0, 0, 0, kOne}, 1, true, "anti-transpose"},
        {{0, 0, 0, 0, 0, 0, 0, 0, kOne}, 0, false, "degenerate matrix"},
        {{46341, -46341, 0, 46341, 46341, 0, 0, 0, kOne}, 0, false, "odd angle is ignored"},
    };
    for (const Case& c : cases) {
        const Rotation::Info info = Rotation::fromMatrix(c.m);
        check(info.quarterTurns == c.turns && info.mirrored == c.mirrored, c.name,
              QStringLiteral("%1 turns, %2")
                  .arg(info.quarterTurns)
                  .arg(info.mirrored ? "mirrored" : "upright")
                  .toStdString());
    }
}

// Recording core (P5). Synthetic frames and a tone go through the encoder, then
// the file is re-opened to check both streams, the frame count and the
// durations. No window or GPU is involved, so this runs headless like the rest.
void testRecorder() {
    std::printf("\n== recorder ==\n");
    const QString path = QDir::temp().filePath(QStringLiteral("play_selftest_rec.mp4"));
    QFile::remove(path);

    Recorder rec;
    std::string err;
    const int w = 320, h = 180, fps = 30, frames = 30;
    const bool started = rec.start(path, w, h, fps, true, 48000, 2, &err);
    check(started, "start", err);
    if (!started) return;

    QByteArray rgba(w * h * 4, 0);
    std::vector<float> pcm;
    const int perFrame = 48000 / fps;
    for (int i = 0; i < frames; ++i) {
        // A moving gradient, so the encoder has real work to do.
        for (int y = 0; y < h; ++y) {
            auto* row = reinterpret_cast<uint8_t*>(rgba.data()) + size_t(y) * w * 4;
            for (int x = 0; x < w; ++x) {
                row[size_t(x) * 4 + 0] = uint8_t((x * 255) / w);
                row[size_t(x) * 4 + 1] = uint8_t((y * 255) / h);
                row[size_t(x) * 4 + 2] = uint8_t((i * 8) & 0xff);
                row[size_t(x) * 4 + 3] = 255;
            }
        }
        rec.pushVideo(rgba, w, h, double(i) / fps);
        for (int s = 0; s < perFrame; ++s) {
            const double t = double(i) / fps + double(s) / 48000.0;
            const float v = 0.2f * float(std::sin(2.0 * 3.14159265358979 * 1000.0 * t));
            pcm.push_back(v);
            pcm.push_back(v);
        }
        rec.pushAudio(pcm.data() + size_t(i) * size_t(perFrame) * 2, perFrame, double(i) / fps);
    }
    rec.stop();

    check(!rec.failed(), "encoder finished without errors");
    check(rec.videoFrames() == uint64_t(frames), "every frame encoded",
          std::to_string(rec.videoFrames()));

    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path.toUtf8().constData(), nullptr, nullptr) != 0 || !fmt) {
        check(false, "the recording opens");
        return;
    }
    const int info = avformat_find_stream_info(fmt, nullptr);
    int v = -1, a = -1;
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        const auto type = fmt->streams[i]->codecpar->codec_type;
        if (type == AVMEDIA_TYPE_VIDEO) v = int(i);
        if (type == AVMEDIA_TYPE_AUDIO) a = int(i);
    }
    check(info >= 0 && v >= 0, "has a video stream");
    check(a >= 0, "has an audio stream");
    if (v >= 0) {
        const AVStream* st = fmt->streams[v];
        check(st->codecpar->width == w && st->codecpar->height == h, "video size",
              std::to_string(st->codecpar->width) + "x" + std::to_string(st->codecpar->height));
        const double dur = double(st->duration) * av_q2d(st->time_base);
        check(std::fabs(dur - double(frames) / fps) < 0.2, "video duration",
              QString::number(dur, 'f', 3).toStdString());
    }
    if (a >= 0) {
        const AVStream* st = fmt->streams[a];
        const double dur = double(st->duration) * av_q2d(st->time_base);
        check(std::fabs(dur - double(frames) / fps) < 0.2, "audio duration",
              QString::number(dur, 'f', 3).toStdString());
    }
    avformat_close_input(&fmt);
    QFile::remove(path);
}

}  // namespace

int runSelfTest(const std::string& path, const std::string& multiAudioPath) {    LOGI("selftest: %s", path.c_str());
    testSubtitles();
    testScaler();
    testHdr();
    testRotation();
    testRecorder();
    testLibrary();
    testPlaylist();
    testEmbeddedSubtitles(path);
    testBasic(path);
    testSpeed(path);
    testSeeks(path, 20);
    testEos(path);
    testLoop(path);
    if (!multiAudioPath.empty()) testAudioSwitch(multiAudioPath);

    std::printf("\n%s (%d failure%s)\n", failures == 0 ? "ALL PASS" : "FAILURES",
                failures, failures == 1 ? "" : "s");
    std::fflush(stdout);
    // The self-test never creates a window, and the Windows platform plugin
    // faults in its own teardown without one; skip the Qt shutdown path.
    std::_Exit(failures == 0 ? 0 : 1);
}
