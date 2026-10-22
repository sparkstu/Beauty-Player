#include "PlaylistModel.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <algorithm>

QStringList PlaylistModel::videoExtensions() {
    return { QStringLiteral("mp4"),  QStringLiteral("mkv"),  QStringLiteral("mov"),
             QStringLiteral("avi"),  QStringLiteral("webm"), QStringLiteral("ts"),
             QStringLiteral("flv"),  QStringLiteral("m4v"),  QStringLiteral("wmv"),
             QStringLiteral("mpg"),  QStringLiteral("mpeg"), QStringLiteral("m2ts"),
             QStringLiteral("ogv"),  QStringLiteral("rmvb") };
}

PlaylistModel::PlaylistModel(QObject* parent) : QAbstractListModel(parent) {}

int PlaylistModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : count();
}

QVariant PlaylistModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= count()) return {};
    const QString& path = items_[static_cast<size_t>(index.row())];
    switch (role) {
    case PathRole:
        return path;
    case NameRole:
        return QFileInfo(path).fileName();
    case DirRole:
        return QFileInfo(path).absolutePath();
    case CurrentRole:
        return index.row() == current_;
    default:
        return {};
    }
}

QHash<int, QByteArray> PlaylistModel::roleNames() const {
    return { { PathRole, "path" },
             { NameRole, "name" },
             { DirRole, "dir" },
             { CurrentRole, "current" } };
}

void PlaylistModel::setCurrent(int row) {
    if (row < -1 || row >= count()) row = -1;
    if (row == current_) return;
    const int old = current_;
    current_ = row;
    if (old >= 0 && old < count()) {
        const QModelIndex i = index(old);
        emit dataChanged(i, i, { CurrentRole });
    }
    if (current_ >= 0) {
        const QModelIndex i = index(current_);
        emit dataChanged(i, i, { CurrentRole });
    }
    emit currentChanged();
}

int PlaylistModel::addPath(const QString& path) {
    const QFileInfo fi(path);
    if (!fi.exists()) return 0;

    QStringList files;
    if (fi.isDir()) {
        const QStringList exts = videoExtensions();
        QDirIterator it(fi.absoluteFilePath(), QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString p = it.next();
            if (exts.contains(QFileInfo(p).suffix().toLower())) files << p;
        }
        files.sort(Qt::CaseInsensitive);
    } else {
        files << fi.absoluteFilePath();
    }
    if (files.isEmpty()) return 0;

    const int first = count();
    beginInsertRows(QModelIndex(), first, first + files.size() - 1);
    for (const QString& f : files) items_.push_back(f);
    endInsertRows();
    emit countChanged();
    if (current_ < 0) setCurrent(first);
    return static_cast<int>(files.size());
}

int PlaylistModel::add(const QList<QUrl>& urls) {
    int added = 0;
    for (const QUrl& u : urls) {
        const QString p = u.isLocalFile() ? u.toLocalFile() : u.toString();
        if (p.isEmpty()) continue;
        added += addPath(p);
    }
    return added;
}

int PlaylistModel::addPaths(const QStringList& paths) {
    int added = 0;
    for (const QString& p : paths) added += addPath(p);
    return added;
}

void PlaylistModel::removeAt(int row) {
    if (row < 0 || row >= count()) return;
    // Keep |current_| on the same file when rows before it disappear.
    const QString keep = current_ >= 0 ? items_[static_cast<size_t>(current_)] : QString();
    beginRemoveRows(QModelIndex(), row, row);
    items_.erase(items_.begin() + row);
    endRemoveRows();
    emit countChanged();

    if (current_ > row) --current_;
    if (current_ == row) current_ = std::min(row, count() - 1);
    if (current_ >= count()) current_ = count() - 1;

    if (!keep.isEmpty()) {
        for (int i = 0; i < count(); ++i) {
            if (items_[static_cast<size_t>(i)] == keep) {
                current_ = i;
                break;
            }
        }
    }
    emit currentChanged();
}

void PlaylistModel::clear() {
    if (items_.empty()) return;
    beginResetModel();
    items_.clear();
    current_ = -1;
    endResetModel();
    emit countChanged();
    emit currentChanged();
}

void PlaylistModel::moveRow(int from, int to) {
    if (from < 0 || from >= count() || to < 0 || to >= count() || from == to) return;
    const QString keep = currentPath();
    beginMoveRows(QModelIndex(), from, from, QModelIndex(), to > from ? to + 1 : to);
    const QString moved = items_[static_cast<size_t>(from)];
    items_.erase(items_.begin() + from);
    items_.insert(items_.begin() + to, moved);
    endMoveRows();
    if (!keep.isEmpty()) setCurrentPath(keep);
}

QString PlaylistModel::pathAt(int row) const {
    if (row < 0 || row >= count()) return {};
    return items_[static_cast<size_t>(row)];
}

QString PlaylistModel::next() {
    if (current_ + 1 >= count()) return {};
    setCurrent(current_ + 1);
    return currentPath();
}

QString PlaylistModel::previous() {
    if (current_ - 1 < 0) return {};
    setCurrent(current_ - 1);
    return currentPath();
}

QString PlaylistModel::currentPath() const {
    return pathAt(current_);
}

void PlaylistModel::setCurrentPath(const QString& path) {
    if (path.isEmpty()) {
        setCurrent(-1);
        return;
    }
    for (int i = 0; i < count(); ++i) {
        if (items_[static_cast<size_t>(i)] == path) {
            setCurrent(i);
            return;
        }
    }
}

bool PlaylistModel::hasNext() const {
    return current_ + 1 < count();
}
