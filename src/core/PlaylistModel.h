#pragma once
#include <QAbstractListModel>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <vector>

// Queue of files to play. Folders are expanded recursively into the video
// extensions in videoExtensions(), sorted by name. The model does not play
// anything itself: QML asks for the next path and assigns video.source.
class PlaylistModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(int current READ current WRITE setCurrent NOTIFY currentChanged)

public:
    enum Roles {
        PathRole = Qt::UserRole + 1,
        NameRole,
        DirRole,
        CurrentRole,
    };

    explicit PlaylistModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int count() const { return static_cast<int>(items_.size()); }
    int current() const { return current_; }
    void setCurrent(int row);

    // Appends everything it can open; returns the number of files added.
    Q_INVOKABLE int add(const QList<QUrl>& urls);
    Q_INVOKABLE int addPath(const QString& path);
    // Same, for local paths (command line arguments, QSettings, ...).
    Q_INVOKABLE int addPaths(const QStringList& paths);
    Q_INVOKABLE void removeAt(int row);
    Q_INVOKABLE void clear();
    Q_INVOKABLE void moveRow(int from, int to);
    Q_INVOKABLE QString pathAt(int row) const;

    // Steps to the next/previous entry and returns its path ("" at the ends).
    Q_INVOKABLE QString next();
    Q_INVOKABLE QString previous();
    // Path of |current| ("" when nothing is selected).
    Q_INVOKABLE QString currentPath() const;

    // Considers |path| the current entry (matches by absolute path).
    Q_INVOKABLE void setCurrentPath(const QString& path);
    // True when at least one more entry follows the current one.
    Q_INVOKABLE bool hasNext() const;

    static QStringList videoExtensions();

signals:
    void countChanged();
    void currentChanged();

private:
    std::vector<QString> items_;
    int current_ = -1;
};
