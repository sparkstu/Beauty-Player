#include "core/Settings.h"

#include <QDir>
#include <QSettings>
#include <QStandardPaths>

#include "core/Library.h"

Settings::Settings(QObject* parent) : QObject(parent) {
    const QString path = defaultPath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    s_ = new QSettings(path, QSettings::IniFormat);

    volume_ = qBound(0.0, s_->value(QStringLiteral("audio/volume"), volume_).toDouble(), 1.0);
    muted_ = s_->value(QStringLiteral("audio/muted"), muted_).toBool();
    speed_ = qBound(0.25, s_->value(QStringLiteral("play/speed"), speed_).toDouble(), 4.0);
    scaler_ = s_->value(QStringLiteral("video/scaler"), scaler_).toString();
    if (scaler_ != QLatin1String("bilinear") && scaler_ != QLatin1String("bicubic")
        && scaler_ != QLatin1String("lanczos3")) {
        scaler_ = QStringLiteral("bilinear");
    }
    loop_ = s_->value(QStringLiteral("play/loop"), loop_).toBool();
    const int w = s_->value(QStringLiteral("window/width"), 1180).toInt();
    const int h = s_->value(QStringLiteral("window/height"), 760).toInt();
    size_ = QSize(qBound(320, w, 8192), qBound(200, h, 8192));
    lastDir_ = s_->value(QStringLiteral("files/lastDir"), QString()).toString();

    // Volume is dragged: coalesce the writes instead of hitting the disk at every
    // mouse move (and every frame while the OSD animates).
    save_.setSingleShot(true);
    save_.setInterval(400);
    connect(&save_, &QTimer::timeout, this, &Settings::save);
}

Settings::~Settings() {
    flush();
    delete s_;
}

QString Settings::defaultPath() {
    // Same folder as the library (AppData/Roaming/play/play on Windows).
    return QFileInfo(Library::defaultPath()).absolutePath() + QStringLiteral("/settings.ini");
}

void Settings::setVolume(qreal v) {
    v = qBound(0.0, v, 1.0);
    if (qFuzzyCompare(volume_ + 0.0001, v + 0.0001)) return;
    volume_ = v;
    emit volumeChanged();
    scheduleSave();
}

void Settings::setMuted(bool on) {
    if (muted_ == on) return;
    muted_ = on;
    emit mutedChanged();
    scheduleSave();
}

void Settings::setSpeed(qreal v) {
    v = qBound(0.25, v, 4.0);
    if (qFuzzyCompare(speed_, v)) return;
    speed_ = v;
    emit speedChanged();
    scheduleSave();
}

void Settings::setScaler(const QString& s) {
    if (scaler_ == s) return;
    scaler_ = s;
    emit scalerChanged();
    scheduleSave();
}

void Settings::setLoop(bool on) {
    if (loop_ == on) return;
    loop_ = on;
    emit loopChanged();
    scheduleSave();
}

void Settings::setWindowWidth(int w) {
    w = qBound(320, w, 8192);
    if (size_.width() == w) return;
    size_ = QSize(w, size_.height());
    emit geometryChanged();
    scheduleSave();
}

void Settings::setWindowHeight(int h) {
    h = qBound(200, h, 8192);
    if (size_.height() == h) return;
    size_ = QSize(size_.width(), h);
    emit geometryChanged();
    scheduleSave();
}

void Settings::setLastDir(const QString& d) {
    if (lastDir_ == d) return;
    lastDir_ = d;
    emit lastDirChanged();
    scheduleSave();
}

void Settings::scheduleSave() {
    if (!save_.isActive()) save_.start();
}

void Settings::flush() {
    if (save_.isActive()) save_.stop();
    save();
}

void Settings::save() {
    if (!s_) return;
    s_->setValue(QStringLiteral("audio/volume"), volume_);
    s_->setValue(QStringLiteral("audio/muted"), muted_);
    s_->setValue(QStringLiteral("play/speed"), speed_);
    s_->setValue(QStringLiteral("play/loop"), loop_);
    s_->setValue(QStringLiteral("video/scaler"), scaler_);
    s_->setValue(QStringLiteral("window/width"), size_.width());
    s_->setValue(QStringLiteral("window/height"), size_.height());
    s_->setValue(QStringLiteral("files/lastDir"), lastDir_);
    s_->sync();
}
