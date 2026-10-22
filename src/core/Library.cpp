#include "Library.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>
#include <algorithm>

#include "Log.h"

namespace {
constexpr size_t kMaxEntries = 40;
}

QString Library::defaultPath() {
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (dir.isEmpty()) dir = QDir::homePath() + QStringLiteral("/.play");
    return dir + QStringLiteral("/library.json");
}

bool Library::load(const QString& path) {
    std::lock_guard<std::mutex> lock(mx_);
    file_ = path;
    entries_.clear();

    QFile f(path);
    if (!f.exists()) return true;  // first run
    if (!f.open(QIODevice::ReadOnly)) {
        LOGW("library: cannot read %s", qPrintable(path));
        return false;
    }
    QJsonParseError perr{};
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &perr);
    f.close();
    if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
        LOGW("library: %s is not valid JSON (%s), starting empty", qPrintable(path),
             qPrintable(perr.errorString()));
        return false;
    }

    const QJsonArray arr = doc.object().value(QStringLiteral("recent")).toArray();
    for (const QJsonValue& v : arr) {
        const QJsonObject o = v.toObject();
        const QString p = o.value(QStringLiteral("path")).toString();
        if (p.isEmpty()) continue;
        Entry e;
        e.path = p;
        e.position = o.value(QStringLiteral("pos")).toDouble(0.0);
        e.duration = o.value(QStringLiteral("dur")).toDouble(0.0);
        e.touched = static_cast<qint64>(o.value(QStringLiteral("at")).toDouble(0));
        entries_.push_back(e);
        if (entries_.size() >= kMaxEntries) break;
    }
    LOGI("library: %zu recent entries from %s", entries_.size(), qPrintable(path));
    return true;
}

bool Library::save(QString* err) const {
    std::lock_guard<std::mutex> lock(mx_);
    if (file_.isEmpty()) {
        if (err) *err = QStringLiteral("库文件路径为空");
        return false;
    }

    QJsonArray arr;
    for (const Entry& e : entries_) {
        QJsonObject o;
        o.insert(QStringLiteral("path"), e.path);
        o.insert(QStringLiteral("pos"), e.position);
        o.insert(QStringLiteral("dur"), e.duration);
        o.insert(QStringLiteral("at"), static_cast<double>(e.touched));
        arr.append(o);
    }
    QJsonObject root;
    root.insert(QStringLiteral("version"), 1);
    root.insert(QStringLiteral("recent"), arr);

    const QFileInfo fi(file_);
    if (!QDir().mkpath(fi.absolutePath())) {
        if (err) *err = QStringLiteral("无法创建目录 %1").arg(fi.absolutePath());
        return false;
    }

    // QSaveFile writes to a temporary and renames on commit, so a crash in the
    // middle cannot truncate the existing library.
    QSaveFile f(file_);
    if (!f.open(QIODevice::WriteOnly)) {
        if (err) *err = f.errorString();
        return false;
    }
    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    if (!f.commit()) {
        if (err) *err = f.errorString();
        return false;
    }
    return true;
}

QVariantList Library::recent(int limit) const {
    std::lock_guard<std::mutex> lock(mx_);
    QVariantList out;
    for (const Entry& e : entries_) {
        if (limit > 0 && out.size() >= limit) break;
        QVariantMap m;
        m.insert(QStringLiteral("path"), e.path);
        m.insert(QStringLiteral("name"), QFileInfo(e.path).fileName());
        m.insert(QStringLiteral("position"), e.position);
        m.insert(QStringLiteral("duration"), e.duration);
        m.insert(QStringLiteral("missing"), !QFileInfo::exists(e.path));
        out.append(m);
    }
    return out;
}

Library::Entry* Library::find(const QString& path) {
    for (Entry& e : entries_) {
        if (e.path == path) return &e;
    }
    return nullptr;
}

void Library::touch(const QString& path, double duration) {
    if (path.isEmpty()) return;
    std::lock_guard<std::mutex> lock(mx_);
    const bool existed = find(path) != nullptr;
    if (existed) {
        Entry moved = *find(path);
        entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                      [&](const Entry& e) { return e.path == path; }),
                       entries_.end());
        entries_.push_front(moved);
    } else {
        Entry e;
        e.path = path;
        entries_.push_front(e);
    }
    Entry& e = entries_.front();
    if (duration > 0.0) e.duration = duration;
    e.touched = QDateTime::currentSecsSinceEpoch();
    while (entries_.size() > kMaxEntries) entries_.pop_back();
}

void Library::setPosition(const QString& path, double position, double duration) {
    if (path.isEmpty()) return;
    std::lock_guard<std::mutex> lock(mx_);
    Entry* e = find(path);
    if (!e) {
        Entry fresh;
        fresh.path = path;
        fresh.touched = QDateTime::currentSecsSinceEpoch();
        entries_.push_front(fresh);
        e = &entries_.front();
        while (entries_.size() > kMaxEntries) entries_.pop_back();
    }
    if (duration > 0.0) e->duration = duration;
    // Near the start or past the credits there is nothing to continue with.
    e->position = shouldResume(position, e->duration) ? position : 0.0;
}

double Library::positionFor(const QString& path) const {
    std::lock_guard<std::mutex> lock(mx_);
    for (const Entry& e : entries_) {
        if (e.path == path) return e.position;
    }
    return 0.0;
}

void Library::remove(const QString& path) {
    std::lock_guard<std::mutex> lock(mx_);
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                  [&](const Entry& e) { return e.path == path; }),
                   entries_.end());
}

void Library::clear() {
    std::lock_guard<std::mutex> lock(mx_);
    entries_.clear();
}

size_t Library::size() const {
    std::lock_guard<std::mutex> lock(mx_);
    return entries_.size();
}

bool Library::shouldResume(double position, double duration, double lead, double tail) {
    if (position < lead) return false;
    if (duration > 0.0 && position > duration - tail) return false;
    return true;
}
