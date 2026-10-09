#include "ui/director/ShotCard.h"

#include "ai/Director.h"
#include "core/JsonUtil.h"
#include "core/Settings.h"
#include "pose/PoseRenderer.h"
#include "ui/Icons.h"
#include "ui/Studio.h"
#include "ui/Theme.h"
#include "ui/dialogs/PoseEditDialog.h"
#include "ui/widgets/Controls.h"
#include "ui/widgets/ThumbnailCache.h"

#include <QCheckBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>

namespace lumen::ui {

pose::Pose ShotCard::shotPose(const ShotState &shot, const QSize &size)
{
    if (shot.editedPose) {
        pose::Pose p = *shot.editedPose;
        p.resizeCanvas(size, true);
        return p;
    }
    return ai::Director::poseForShot(shot.shot, size);
}

ShotCard::ShotCard(Studio *studio, DirectorSession *session, const QString &shotId, ThumbnailCache *thumbs, QWidget *parent)
    : QFrame(parent)
    , m_studio(studio)
    , m_session(session)
    , m_id(shotId)
    , m_thumbs(thumbs)
{
    setProperty("card", QStringLiteral("true"));
    auto *h = new QHBoxLayout(this);
    h->setContentsMargins(12, 12, 12, 12);
    h->setSpacing(14);
    m_poseThumb = new QToolButton(this);
    m_poseThumb->setFixedSize(120, 160);
    m_poseThumb->setIconSize(QSize(112, 152));
    m_poseThumb->setToolTip(tr("Edit the pose"));
    m_poseThumb->setStyleSheet(QStringLiteral("QToolButton { background: #000; border-radius: 8px; border: 1px solid %1; }"
                                              "QToolButton:hover { border-color: %2; }")
                                   .arg(Theme::palette().border.name(), Theme::palette().accent.name()));
    h->addWidget(m_poseThumb, 0, Qt::AlignTop);
    auto *v = new QVBoxLayout;
    v->setSpacing(6);
    auto *head = new QHBoxLayout;
    m_include = new QCheckBox(this);
    m_include->setToolTip(tr("Include in 'Generate all'"));
    m_title = new QLabel(this);
    Theme::setRole(m_title, QStringLiteral("heading"));
    m_badge = new QLabel(this);
    Theme::setRole(m_badge, QStringLiteral("badge"));
    head->addWidget(m_include);
    head->addWidget(m_title, 1);
    head->addWidget(m_badge);
    v->addLayout(head);
    m_description = mutedLabel(QString(), this);
    v->addWidget(m_description);
    m_prompt = new AutoTextEdit(this, 2, 6);
    m_prompt->setPlaceholderText(tr("Prompt"));
    v->addWidget(m_prompt);
    auto *actions = new QHBoxLayout;
    auto *poseBtn = new QPushButton(Icons::get(QStringLiteral("person")), tr("Edit pose"), this);
    m_generate = new QPushButton(Icons::get(QStringLiteral("sparkles")), tr("Generate"), this);
    Theme::setRole(m_generate, QStringLiteral("primary"));
    m_status = mutedLabel(QString(), this);
    actions->addWidget(poseBtn);
    actions->addWidget(m_generate);
    actions->addWidget(m_status, 1);
    v->addLayout(actions);
    m_results = new QHBoxLayout;
    m_results->setSpacing(6);
    v->addLayout(m_results);
    h->addLayout(v, 1);

    connect(m_poseThumb, &QToolButton::clicked, this, &ShotCard::editPose);
    connect(poseBtn, &QPushButton::clicked, this, &ShotCard::editPose);
    connect(m_generate, &QPushButton::clicked, this, [this] { emit generateRequested(m_id); });
    connect(m_include, &QCheckBox::toggled, this, [this](bool on) {
        if (ShotState *s = m_session ? m_session->shot(m_id) : nullptr) {
            s->approved = on;
            m_session->saveSoon();
        }
    });
    connect(m_prompt, &QPlainTextEdit::textChanged, this, [this] {
        if (ShotState *s = m_session ? m_session->shot(m_id) : nullptr) {
            if (s->shot.value(QStringLiteral("prompt")).toString() != m_prompt->text()) {
                s->shot.insert(QStringLiteral("prompt"), m_prompt->text());
                m_session->saveSoon();
            }
        }
    });
    connect(thumbs, &ThumbnailCache::ready, this, [this](const QString &id) {
        if (ShotState *s = m_session ? m_session->shot(m_id) : nullptr; s && s->results.contains(id))
            refreshResults();
    });
    refresh();
}

void ShotCard::refresh()
{
    ShotState *s = m_session ? m_session->shot(m_id) : nullptr;
    if (!s)
        return;
    m_title->setText(s->shot.value(QStringLiteral("title")).toString());
    const pose::RigParams rig = s->rig();
    m_badge->setText(QStringLiteral("%1 · %2").arg(s->aspect(), QString(rig.framing).replace(u'_', u' ')));
    QString desc = s->shot.value(QStringLiteral("description")).toString();
    const QString bg = s->shot.value(QStringLiteral("background")).toString();
    if (!bg.isEmpty())
        desc += QStringLiteral("\n") + tr("Background: %1").arg(bg);
    m_description->setText(desc);
    {
        QSignalBlocker b(m_prompt);
        m_prompt->setPlainText(s->shot.value(QStringLiteral("prompt")).toString());
    }
    {
        QSignalBlocker b(m_include);
        m_include->setChecked(s->approved);
    }
    const QSize thumb = ai::Director::sizeForAspect(s->aspect(), 112 * 152, 8);
    const pose::Pose p = shotPose(*s, thumb * 2);
    pose::RenderOptions opts;
    opts.thickness = 1.2;
    m_poseThumb->setIcon(QIcon(QPixmap::fromImage(pose::renderOpenPose(p, p.canvas, opts))));
    refreshResults();
}

void ShotCard::refreshResults()
{
    while (QLayoutItem *item = m_results->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    ShotState *s = m_session ? m_session->shot(m_id) : nullptr;
    if (!s)
        return;
    for (const QString &id : s->results) {
        auto *b = new QToolButton(this);
        b->setFixedSize(84, 84);
        b->setIconSize(QSize(80, 80));
        const QPixmap pm = m_thumbs->get(id);
        if (!pm.isNull())
            b->setIcon(QIcon(pm));
        b->setToolTip(tr("Open in editor (right-click for more)"));
        const QJsonObject verdict = s->critiques.value(id).toObject();
        if (!verdict.isEmpty()) {
            const int score = verdict.value(QStringLiteral("score")).toInt(ai::Director::critiqueScore(verdict));
            const int threshold = Settings::instance().value(QStringLiteral("director/criticThreshold"), 7).toInt();
            QStringList issues = json::toStringList(verdict.value(QStringLiteral("issues")).toArray()).mid(0, 4);
            for (QString &issue : issues)
                issue.prepend(QStringLiteral("• "));
            b->setToolTip(tr("Critic: %1/10 (identity %2, pose %3)").arg(score).arg(verdict.value(QStringLiteral("identity")).toInt())
                              .arg(verdict.value(QStringLiteral("pose")).toInt())
                          + (issues.isEmpty() ? QString() : u'\n' + issues.join(u'\n')) + QStringLiteral("\n\n") + b->toolTip());
            b->setStyleSheet(QStringLiteral("QToolButton { border: 2px solid %1; border-radius: 6px; }")
                                 .arg((score >= threshold ? Theme::palette().success : Theme::palette().warning).name()));
            b->setAccessibleDescription(tr("Critic score %1 of 10").arg(score));
        }
        b->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(b, &QToolButton::clicked, this, [this, id] { m_studio->openAssetInEditor(id); });
        connect(b, &QToolButton::customContextMenuRequested, this, [this, b, id](const QPoint &pos) {
            QMenu menu(this);
            menu.addAction(Icons::get(QStringLiteral("brush")), tr("Open in editor"), this, [this, id] { m_studio->openAssetInEditor(id); });
            menu.addAction(Icons::get(QStringLiteral("link")), tr("Use as character reference"), this, [this, id] {
                if (m_session)
                    m_session->addReferences({id});
            });
            menu.addAction(Icons::get(QStringLiteral("close")), tr("Remove from shot"), this, [this, id] {
                if (ShotState *st = m_session ? m_session->shot(m_id) : nullptr) {
                    st->results.removeAll(id);
                    m_session->saveSoon();
                    refreshResults();
                }
            });
            menu.exec(b->mapToGlobal(pos));
        });
        m_results->addWidget(b);
    }
    m_results->addStretch();
}

void ShotCard::setBusy(bool busy, const QString &status)
{
    m_generate->setEnabled(!busy);
    m_status->setText(status);
}

void ShotCard::editPose()
{
    ShotState *s = m_session ? m_session->shot(m_id) : nullptr;
    if (!s)
        return;
    const QSize size = ai::Director::sizeForAspect(s->aspect(), 1024 * 1024, 64);
    const std::optional<pose::RigParams> rig = s->editedPose ? std::nullopt : std::optional<pose::RigParams>(s->rig());
    PoseEditDialog dlg(m_studio, shotPose(*s, size), rig, s->shot.value(QStringLiteral("description")).toString(), this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    s = m_session ? m_session->shot(m_id) : nullptr;
    if (!s)
        return;
    if (dlg.rig()) {
        s->shot.insert(QStringLiteral("pose"), dlg.rig()->toJson());
        s->editedPose.reset();
    } else {
        s->editedPose = dlg.pose();
    }
    m_session->notifyShotChanged(m_id);
    refresh();
}

} // namespace lumen::ui
