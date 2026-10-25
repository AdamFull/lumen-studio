#pragma once

#include "plugins/characters/director/DirectorSession.h"

#include <QFrame>
#include <QPointer>

class QCheckBox;
class QHBoxLayout;
class QLabel;
class QPushButton;
class QToolButton;

namespace lumen::ui {

class AutoTextEdit;
class Studio;
class ThumbnailCache;

// One planned shot: pose preview, prompt, generate button and results.
class ShotCard : public QFrame
{
    Q_OBJECT
public:
    ShotCard(Studio *studio, DirectorSession *session, const QString &shotId, ThumbnailCache *thumbs, QWidget *parent = nullptr);
    QString shotId() const { return m_id; }
    void refresh();
    void setBusy(bool busy, const QString &status = QString());

    // Pose for generation (edited keypoints or the solved rig) at the given size.
    static pose::Pose shotPose(const ShotState &shot, const QSize &size);

signals:
    void generateRequested(const QString &shotId);

private:
    void editPose();
    void refreshResults();

    Studio *m_studio;
    QPointer<DirectorSession> m_session;
    QString m_id;
    ThumbnailCache *m_thumbs;
    QToolButton *m_poseThumb;
    QLabel *m_title;
    QLabel *m_badge;
    QLabel *m_description;
    AutoTextEdit *m_prompt;
    QCheckBox *m_include;
    QPushButton *m_generate;
    QLabel *m_status;
    QHBoxLayout *m_results;
};

} // namespace lumen::ui
