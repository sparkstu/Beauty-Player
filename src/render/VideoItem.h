#pragma once
#include <QQuickRhiItem>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <atomic>
#include <memory>

#include "core/Library.h"
#include "core/Player.h"
#include "core/SubtitleTrack.h"
#include "Scaler.h"

// Qt Quick item that renders the decoded video through a custom RHI pass.
// The shader chain here is where filter/effect passes are inserted later; the
// beauty chain is deferred for now, the interface stays.
class VideoItem : public QQuickRhiItem {
    Q_OBJECT
    Q_PROPERTY(QString source READ source WRITE setSource NOTIFY sourceChanged)
    Q_PROPERTY(QString errorString READ errorString NOTIFY errorStringChanged)
    Q_PROPERTY(bool playing READ playing NOTIFY playingChanged)
    Q_PROPERTY(bool finished READ isFinished NOTIFY finishedChanged)
    Q_PROPERTY(bool loop READ isLooping WRITE setLoop NOTIFY loopChanged)
    Q_PROPERTY(qreal position READ position NOTIFY positionChanged)
    Q_PROPERTY(qreal duration READ duration NOTIFY durationChanged)
    Q_PROPERTY(bool hardwareDecoding READ hardwareDecoding NOTIFY sourceChanged)
    Q_PROPERTY(QVariantList audioTracks READ audioTracks NOTIFY audioTracksChanged)
    // Placeholder colour controls that drive the effect pass; the beauty chain
    // (deferred) will reuse the same property -> render-thread path.
    Q_PROPERTY(qreal saturation READ saturation WRITE setSaturation NOTIFY saturationChanged)
    Q_PROPERTY(qreal brightness READ brightness WRITE setBrightness NOTIFY brightnessChanged)
    // Playback rate, 0.25..4. Audio is time-stretched (atempo), pitch kept.
    Q_PROPERTY(qreal playbackSpeed READ playbackSpeed WRITE setPlaybackSpeed NOTIFY playbackSpeedChanged)
    // Resampling filter used when the output rectangle != video size:
    // "bilinear" (hardware sampler, default), "bicubic", "lanczos3".
    Q_PROPERTY(QString scaler READ scaler WRITE setScaler NOTIFY scalerChanged)
    // True when the window should render into a scRGB (FP16 linear) swapchain.
    // Only takes effect when Windows HDR is on for the window's display; Qt
    // falls back to SDR otherwise (see --hdr).
    Q_PROPERTY(bool hdr READ hdr WRITE setHdr NOTIFY hdrChanged)
    // Output volume, 0..1 (device-side, so it costs nothing per frame).
    Q_PROPERTY(qreal volume READ volume WRITE setVolume NOTIFY volumeChanged)
    Q_PROPERTY(bool muted READ muted NOTIFY volumeChanged)
    // --- subtitles (sidecar SRT/VTT/ASS) ---
    Q_PROPERTY(bool subtitlesEnabled READ subtitlesEnabled WRITE setSubtitlesEnabled NOTIFY subtitlesChanged)
    Q_PROPERTY(QString subtitleLabel READ subtitleLabel NOTIFY subtitlesChanged)
    Q_PROPERTY(bool hasSubtitles READ hasSubtitles NOTIFY subtitlesChanged)
    Q_PROPERTY(QStringList subtitleFiles READ subtitleFiles NOTIFY subtitlesChanged)
    // Subtitle streams inside the container; each entry has index/label/codec/
    // language/title/text (text == false for bitmap formats).
    Q_PROPERTY(QVariantList embeddedSubtitles READ embeddedSubtitles NOTIFY subtitlesChanged)
    // Recently played files ({path,name,position,duration,missing}), newest first.
    Q_PROPERTY(QVariantList recentFiles READ recentFiles NOTIFY recentChanged)
    // --- recording (P5) ---
    // True while the renderer is feeding the encoder. Cleared automatically when
    // the encoder fails or the file is closed.
    Q_PROPERTY(bool recording READ recording NOTIFY recordingChanged)
    // File being written (empty when not recording).
    Q_PROPERTY(QString recordingFile READ recordingFile NOTIFY recordingChanged)
    // Clip export (P5.2): running, plus the A/B marks in seconds.
    Q_PROPERTY(bool exporting READ exporting NOTIFY exportingChanged)
    Q_PROPERTY(qreal exportStart READ exportStart NOTIFY exportMarksChanged)
    Q_PROPERTY(qreal exportEnd READ exportEnd NOTIFY exportMarksChanged)
    Q_PROPERTY(qreal exportProgress READ exportProgress NOTIFY exportProgressChanged)

public:
    explicit VideoItem(QQuickItem* parent = nullptr);
    ~VideoItem() override;

    QQuickRhiItemRenderer* createRenderer() override;

    QString source() const { return source_; }
    void setSource(const QString& s);

    QString errorString() const { return error_; }
    bool playing() const;
    bool isFinished() const;
    bool isLooping() const;
    void setLoop(bool on);
    qreal position() const;
    qreal duration() const;
    bool hardwareDecoding() const;
    QVariantList audioTracks() const;

    qreal saturation() const { return saturation_; }
    void setSaturation(qreal v);
    qreal brightness() const { return brightness_; }
    void setBrightness(qreal v);
    qreal playbackSpeed() const { return playbackSpeed_; }
    void setPlaybackSpeed(qreal v);
    QString scaler() const { return QString::fromLatin1(Scaler::name(scalerMode_)); }
    void setScaler(const QString& name);
    // Render-thread side: the enum value behind |scaler|.
    int scalerMode() const { return scalerMode_; }
    bool hdr() const { return hdr_; }
    void setHdr(bool on);
    qreal volume() const { return volume_; }
    void setVolume(qreal v);
    // Muting keeps |volume_| so unmuting restores the old level.
    bool muted() const { return volume_ <= 0.0001; }
    Q_INVOKABLE void toggleMute();

    bool subtitlesEnabled() const { return subtitlesEnabled_; }
    void setSubtitlesEnabled(bool on);
    QString subtitleLabel() const;
    bool hasSubtitles() const;
    QStringList subtitleFiles() const { return subFiles_; }
    QVariantList embeddedSubtitles() const { return embeddedList_; }
    QVariantList recentFiles() const { return library_.recent(10); }
    // Drops one entry from the recent list (does not touch the file).
    Q_INVOKABLE void forgetRecent(const QString& path);
    Q_INVOKABLE void clearRecent();
    // Called from the render thread with the master clock position.
    QString subtitleTextAt(double t) const;

    Player* player() const { return player_.get(); }

    Q_INVOKABLE void togglePlay();
    Q_INVOKABLE void seekTo(qreal seconds);
    Q_INVOKABLE void open(const QUrl& url);
    Q_INVOKABLE void selectAudioTrack(int streamIndex);
    Q_INVOKABLE bool openSubtitle(const QUrl& url);
    // Decodes subtitle stream |streamIndex| of the current source.
    Q_INVOKABLE bool selectEmbeddedSubtitle(int streamIndex);
    Q_INVOKABLE void clearSubtitle();
    Q_INVOKABLE void toggleSubtitles();

    // --- recording (P5) ---
    bool recording() const { return recording_; }
    QString recordingFile() const { return recordingFile_; }
    // Starts capturing the playback to |path| (MP4). An empty |path| picks a
    // name next to the source file. Returns false and sets errorString() when
    // the encoder cannot start (e.g. no write permission).
    Q_INVOKABLE bool startRecording(const QString& path = QString());
    Q_INVOKABLE void stopRecording();
    Q_INVOKABLE void toggleRecording();

    // --- clip export (P5.2) ---    // Marks the in/out points (seconds; the current position if |seconds| < 0).
    Q_INVOKABLE void markExportStart(qreal seconds = -1);
    Q_INVOKABLE void markExportEnd(qreal seconds = -1);
    // Records A..B to a file next to the source. Playback runs in real time
    // (the clip is captured from the render chain, like a normal recording),
    // then stops by itself at B.
    Q_INVOKABLE bool exportClip();
    Q_INVOKABLE void cancelExport();
    // --export: starts the clip export as soon as the duration is known.
    Q_INVOKABLE void exportWhenReady(qreal a, qreal b);
    bool exporting() const { return exportStage_ != ExportStage::Idle; }
    qreal exportStart() const { return exportStart_; }
    qreal exportEnd() const { return exportEnd_; }
    qreal exportProgress() const { return exportProgress_; }
    // Offline export: the item pulls frames itself and the renderer must use
    // them instead of the playback clock's frame.
    bool offlineExport() const { return offlineExport_; }
    VideoFramePtr offlineFrame() const { return offlineFrame_; }

    // --- screenshot (P5) ---
    // Saves the current frame as a PNG through the recording chain, so it gets
    // the same HDR->SDR mapping as a recording and no window chrome. An empty
    // |path| picks a name next to the source file.
    Q_INVOKABLE bool saveScreenshot(const QString& path = QString());
    // Called by the renderer (via synchronize) to claim a pending request. The
    // GUI thread owns the queue, so this is a plain member guarded by the fact
    // that only one side touches it per frame.
    QString takeScreenshotRequest();

signals:
    void sourceChanged();
    void errorStringChanged();
    void playingChanged();
    void finishedChanged();
    void loopChanged();
    void positionChanged();
    void durationChanged();
    void audioTracksChanged();
    void saturationChanged();
    void brightnessChanged();
    void playbackSpeedChanged();
    void scalerChanged();
    void hdrChanged();
    void volumeChanged();
    void subtitlesChanged();
    void recentChanged();
    void recordingChanged();
    void exportMarksChanged();
    void exportingChanged();
    void exportProgressChanged();
    // ok == false means the export was cancelled or could not start.
    void exportFinished(const QString& file, bool ok);
    // Screenshot written (or failed: |ok| false, |file| may be empty).
    void screenshotSaved(const QString& file, bool ok);

private:
    void startPlayback(const QString& path);
    // The public startRecording() refuses while an export owns the recorder;
    // the export uses this one.
    bool startRecordingInternal(const QString& path);
    // Closes the encoder, restores the player and reports the result.
    void finishExport(bool ok);
    // Runs one slice of an offline export; true when it owned this tick.
    bool tickOfflineExport();
    void setError(const QString& e);
    // Writes the current position of |source_| into the library.
    void persistPosition();
    // Called when the file plays to the end: the next open starts from zero.
    void markWatched();
    // Sidecar files next to |path|: "movie.srt", "movie.zh.srt", ...
    static QStringList findSidecarSubtitles(const QString& path);

    std::unique_ptr<Player> player_;
    QString source_;
    QString error_;
    QTimer tick_;
    int tickCount_ = 0;
    bool finished_ = false;
    bool loop_ = false;
    qreal saturation_ = 1.0;
    qreal brightness_ = 0.0;
    qreal playbackSpeed_ = 1.0;
    int scalerMode_ = Scaler::Bilinear;
    bool hdr_ = false;
    qreal volume_ = 1.0;
    qreal volumeBeforeMute_ = 1.0;
    Library library_;
    bool noResume_ = false;
    int saveTick_ = 0;
    QString lastPersistPath_;
    double lastPersistPos_ = -1.0;

    SubtitleTrack subs_;
    QStringList subFiles_;
    std::vector<SubtitleTrack::EmbeddedTrack> embeddedSubs_;
    QVariantList embeddedList_;
    bool subtitlesEnabled_ = true;
    bool recording_ = false;
    QString recordingFile_;

    // Clip export: a tiny staged state machine driven by the tick, so the
    // recorder only starts once the seek to A has actually produced a frame.
    enum class ExportStage { Idle, Seeking, Recording };
    ExportStage exportStage_ = ExportStage::Idle;
    qreal exportStart_ = 0.0;
    qreal exportEnd_ = 0.0;
    QString exportFile_;
    qreal exportResumeSpeed_ = 1.0;
    qint64 exportSeekMs_ = 0;
    bool pendingExport_ = false;
    qreal pendingExportA_ = 0.0;
    qreal pendingExportB_ = 0.0;
    // Offline (fast) export: frames are pulled from the decoder and rendered by
    // the item, so the clip is produced as fast as the GPU and encoder allow.
    bool offlineExport_ = false;
    VideoFramePtr offlineFrame_;        // last exported frame (preview)
    VideoFramePtr pendingOfflineFrame_; // pulled, not rendered yet (retry)
    // Screenshot: the GUI thread queues a path, the renderer claims it in
    // synchronize() and writes the file from the readback.
    QString shotRequest_;
    qreal exportProgress_ = 0.0;

    // Set by createRenderer(): the item drives offscreen frames through it when
    // the window is hidden (see the tick). The handle is shared so a destroyed
    // renderer (device loss) can clear it without touching the item.
    std::shared_ptr<std::atomic<QQuickRhiItemRenderer*>> rendererHandle_;
    // beforeRendering/afterRendering hooks that hold the frame lock.
    QMetaObject::Connection frameLockBegin_;
    QMetaObject::Connection frameLockEnd_;
};
