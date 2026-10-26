#pragma once

#include <QHash>
#include <QPixmap>
#include <QSet>
#include <QTimer>
#include <QWidget>

class QFrame;
class QLabel;
class QToolButton;

namespace lumen::ui {

class FlowLayout;
class Studio;
class ThumbnailCache;

// What the editor shows while no document is open: ways to start (open, paste, new from a prompt,
// a blank canvas), the image files opened recently, the project's recent documents and images, and
// "Set up AI" while no image engine is configured. The editor page hosts it; the main window
// answers its requests.
class StartScreen : public QWidget
{
    Q_OBJECT
public:
    explicit StartScreen(Studio *studio, QWidget *parent = nullptr);

    // Tiles of the recent documents and images, newest first (tests).
    QList<QToolButton *> recentTiles() const;
    // Tiles of the recent image files (File → Open recent) that are still there, newest first.
    QList<QToolButton *> recentFileTiles() const;

signals:
    void openImageRequested();
    void pasteRequested();
    void newFromPromptRequested();
    void newCanvasRequested();
    void browseRequested();
    void setUpAiRequested();

protected:
    void showEvent(QShowEvent *e) override;

private:
    void rebuildRecent();
    void scheduleRebuild();
    void updateSetupCard();
    QToolButton *addTile(FlowLayout *tiles, const QString &name, const QString &tooltip, const QPixmap &thumbnail,
                         bool document = false);
    // Reads a small picture of an image file in the background, once per file and change.
    void loadFileThumbnail(const QString &path);
    static QString thumbnailKey(const QString &path);

    Studio *m_studio;
    ThumbnailCache *m_thumbs;
    QFrame *m_setupCard;
    QWidget *m_filesBox;
    QWidget *m_fileTilesHost;
    FlowLayout *m_fileTiles;
    QHash<QString, QPixmap> m_fileThumbs; // by thumbnailKey
    QSet<QString> m_loading;
    QWidget *m_recentBox;
    QWidget *m_tilesHost;
    FlowLayout *m_tiles;
    QLabel *m_noRecent;
    QTimer m_rebuild;
    bool m_dirty = true;
};

} // namespace lumen::ui
