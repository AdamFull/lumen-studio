#pragma once

#include <QHash>
#include <QSet>
#include <QWidget>

class QFrame;
class QLabel;
class QMenu;
class QPushButton;

namespace lumen::ai {
class Provider;
}

namespace lumen::ui {

class ElidedLabel;
class Studio;

// Settings → AI (plan M17 §17.5), the first tab: one list in plain words of what does the AI
// work: the image engine, an optional separate engine for changes to an image, and the language
// model while a plugin that needs one is on. Each with its status, its model (the preset's name
// when Lumen knows it) and Change… (another configured engine, or the setup wizard). The
// Providers, Local servers and Models tabs keep every setting for advanced use.
class AiSettingsPage : public QWidget
{
    Q_OBJECT
public:
    explicit AiSettingsPage(Studio *studio, QWidget *parent = nullptr);

    void refresh();

signals:
    // Another tab of the Settings dialog should open ("roles").
    void tabRequested(const QString &id);

protected:
    void showEvent(QShowEvent *e) override;

private:
    struct Row
    {
        QFrame *box = nullptr;
        ElidedLabel *engine = nullptr;
        ElidedLabel *model = nullptr;
        QLabel *status = nullptr;
        QPushButton *change = nullptr;
        QMenu *menu = nullptr;
    };
    Row makeRow(const QString &id, const QString &title, const QString &text);
    void fill(Row &row, ai::Provider *provider, const QString &none, bool edits);
    QString statusOf(ai::Provider *provider, bool *good);
    QString modelOf(ai::Provider *provider, bool edits) const;
    void fillImageMenu(QMenu *menu, bool edits);
    void fillLanguageMenu(QMenu *menu);
    void openWizard();

    Studio *m_studio;
    Row m_image;
    Row m_edit;
    Row m_language;
    QLabel *m_languageUsers = nullptr;
    bool m_shown = false;              // connections are checked once the page is seen
    QHash<QString, QString> m_checked; // provider id → result of its connection test
    QHash<QString, bool> m_reachable;
    QSet<QString> m_checking;
};

} // namespace lumen::ui
