#pragma once
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <deque>
#include <mutex>

// Recently played files and their resume positions, persisted as JSON next to
// the app's settings. GUI thread only; the file write is atomic (write to a
// temporary file, then rename).
class Library {
public:
    struct Entry {
        QString path;
        double position = 0.0;
        double duration = 0.0;
        qint64 touched = 0;  // epoch seconds, most recent first
    };

    // %APPDATA%/play/library.json on Windows.
    static QString defaultPath();

    bool load(const QString& path);
    bool save(QString* err = nullptr) const;

    const QString& path() const { return file_; }
    void setPath(const QString& file) { file_ = file; }

    // Most recent first, capped at |limit|.
    QVariantList recent(int limit = 12) const;
    // Adds/moves |path| to the front. Keeps an existing position.
    void touch(const QString& path, double duration = -1.0);
    // Stores the resume position; a position in the first/last seconds is
    // treated as "finished" and dropped.
    void setPosition(const QString& path, double position, double duration);
    double positionFor(const QString& path) const;
    void remove(const QString& path);
    void clear();

    size_t size() const;
    const std::deque<Entry>& entries() const { return entries_; }

    // Resume when there is something substantial left. |lead| and |tail|
    // seconds near the start/end are ignored.
    static bool shouldResume(double position, double duration, double lead = 5.0,
                             double tail = 10.0);

private:
    Entry* find(const QString& path);

    mutable std::mutex mx_;
    std::deque<Entry> entries_;
    QString file_;
};
