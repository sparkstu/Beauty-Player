#pragma once

#include <QObject>
#include <QSize>
#include <QString>
#include <QTimer>

class QSettings;

// User preferences that survive a restart: volume, rate, scaler, loop, the
// window size and the last folder used in the file dialog. Kept in
// settings.ini next to the recently-played library so everything the app
// remembers lives in one folder.
//
// Command-line flags (--speed, --scaler, ...) are applied after these values and
// therefore win for the session; the QML side writes a change back when the user
// makes one, so the CLI value becomes the stored value only if it differs.
class Settings : public QObject {
    Q_OBJECT
    Q_PROPERTY(qreal volume READ volume WRITE setVolume NOTIFY volumeChanged)
    Q_PROPERTY(bool muted READ muted WRITE setMuted NOTIFY mutedChanged)
    Q_PROPERTY(qreal speed READ speed WRITE setSpeed NOTIFY speedChanged)
    Q_PROPERTY(QString scaler READ scaler WRITE setScaler NOTIFY scalerChanged)
    Q_PROPERTY(bool loop READ loop WRITE setLoop NOTIFY loopChanged)
    Q_PROPERTY(int windowWidth READ windowWidth WRITE setWindowWidth NOTIFY geometryChanged)
    Q_PROPERTY(int windowHeight READ windowHeight WRITE setWindowHeight NOTIFY geometryChanged)
    Q_PROPERTY(QString lastDir READ lastDir WRITE setLastDir NOTIFY lastDirChanged)

public:
    explicit Settings(QObject* parent = nullptr);
    ~Settings() override;

    static QString defaultPath();

    qreal volume() const { return volume_; }
    void setVolume(qreal v);
    bool muted() const { return muted_; }
    void setMuted(bool on);
    qreal speed() const { return speed_; }
    void setSpeed(qreal v);
    QString scaler() const { return scaler_; }
    void setScaler(const QString& s);
    bool loop() const { return loop_; }
    void setLoop(bool on);
    int windowWidth() const { return size_.width(); }
    int windowHeight() const { return size_.height(); }
    void setWindowWidth(int w);
    void setWindowHeight(int h);
    QString lastDir() const { return lastDir_; }
    void setLastDir(const QString& d);

    // Writes anything still pending (call on exit).
    void flush();

signals:
    void volumeChanged();
    void mutedChanged();
    void speedChanged();
    void scalerChanged();
    void loopChanged();
    void geometryChanged();
    void lastDirChanged();

private:
    void scheduleSave();
    void save();

    QSettings* s_ = nullptr;
    QTimer save_;
    qreal volume_ = 0.8;
    bool muted_ = false;
    qreal speed_ = 1.0;
    QString scaler_ = QStringLiteral("bilinear");
    bool loop_ = false;
    QSize size_;
    QString lastDir_;
};
