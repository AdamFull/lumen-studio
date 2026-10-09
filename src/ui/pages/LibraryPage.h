#pragma once

#include "ai/Types.h"
#include "doc/Project.h"

#include <QAbstractListModel>
#include <QPointer>
#include <QSortFilterProxyModel>
#include <QWidget>

class QButtonGroup;
class QLabel;
class QLineEdit;
class QListView;
class QPlainTextEdit;
class QPushButton;
class QSlider;
class QStackedWidget;
class QToolButton;

namespace lumen::ui {

class Studio;
class ThumbnailCache;

// List model over the project's assets (newest first).
class AssetModel : public QAbstractListModel
{
    Q_OBJECT
public:
    enum Roles { IdRole = Qt::UserRole + 1, KindRole, FavoriteRole, InfoRole, SearchRole, CreatedRole };
    AssetModel(ThumbnailCache *thumbs, QObject *parent = nullptr);
    void setProject(doc::Project *project);
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;
    QStringList mimeTypes() const override;
    QMimeData *mimeData(const QModelIndexList &indexes) const override;
    QModelIndex indexOf(const QString &id) const;

private:
    void reload();
    QPointer<doc::Project> m_project;
    QList<doc::AssetInfo> m_assets;
    ThumbnailCache *m_thumbs;
};

class AssetFilter : public QSortFilterProxyModel
{
    Q_OBJECT
public:
    using QSortFilterProxyModel::QSortFilterProxyModel;
    void setKind(int kind); // -1 = all
    void setFavoritesOnly(bool on);
    void setSearch(const QString &text);

protected:
    bool filterAcceptsRow(int row, const QModelIndex &parent) const override;

private:
    int m_kind = -1;
    bool m_favorites = false;
    QString m_search;
};

class LibraryPage : public QWidget
{
    Q_OBJECT
public:
    explicit LibraryPage(Studio *studio, QWidget *parent = nullptr);

private:
    void showInspector();
    QStringList selectedIds() const;
    void exportSelected();
    void deleteSelected();
    // Runs a provider utility on every selected asset; each result becomes a new asset.
    void runBatchUtility(ai::Utility utility, const QJsonObject &params, const QString &title);
    void rebuildDocuments();

    Studio *m_studio;
    ThumbnailCache *m_thumbs;
    AssetModel *m_model;
    AssetFilter *m_filter;
    QListView *m_view;
    QStackedWidget *m_center;
    QWidget *m_docsView;
    QButtonGroup *m_kinds;
    QLineEdit *m_search;
    QToolButton *m_favorites;
    QSlider *m_size;
    // Inspector.
    QLabel *m_preview;
    QLineEdit *m_name;
    QLabel *m_meta;
    QPlainTextEdit *m_prompt;
    QPlainTextEdit *m_params;
    QToolButton *m_star;
    QWidget *m_inspector;
    QString m_currentId;
    quint64 m_previewGeneration = 0;
};

} // namespace lumen::ui
