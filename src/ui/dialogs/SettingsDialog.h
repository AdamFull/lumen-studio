#pragma once

#include "ai/Provider.h"

#include <QDialog>
#include <QHash>
#include <QPointer>

class QComboBox;
class QFormLayout;
class QLabel;
class QLineEdit;
class QListWidget;
class QStackedWidget;
class QTabWidget;
class QVBoxLayout;

namespace lumen::ui {

class Studio;

// Editor for one provider instance, generated from its ProviderType settings schema.
class ProviderForm : public QWidget
{
    Q_OBJECT
public:
    ProviderForm(Studio *studio, ai::Provider *provider, QWidget *parent = nullptr);
    QVariantMap values() const;
    QString name() const;

signals:
    void saved();

private:
    QWidget *createField(const ai::SettingField &field, const QVariant &value);
    void refreshModels(const QString &key);
    void test();
    void save();

    Studio *m_studio;
    QPointer<ai::Provider> m_provider;
    QLineEdit *m_name;
    QHash<QString, QWidget *> m_fields;
    QComboBox *m_server = nullptr; // managed server binding (local server kinds only)
    QLabel *m_status;
};

class SettingsDialog : public QDialog
{
    Q_OBJECT
public:
    explicit SettingsDialog(Studio *studio, QWidget *parent = nullptr);
    void showTab(const QString &tab);

private:
    QWidget *buildProvidersTab();
    QWidget *buildRolesTab();
    QWidget *buildGeneralTab();
    QWidget *buildPluginsTab();
    void addTab(const QString &id, QWidget *page, const QString &icon, const QString &label);
    void reloadProviders(const QString &select = QString());
    void showProvider(const QString &id);
    void addProvider(const QString &typeId);

    Studio *m_studio;
    QTabWidget *m_tabs;
    QStringList m_tabIds; // showTab ids, in tab order
    QListWidget *m_list;
    QVBoxLayout *m_formHost;
    QPointer<QWidget> m_form;
};

// First-run assistant: detects local servers and helps adding providers.
class OnboardingDialog : public QDialog
{
    Q_OBJECT
public:
    explicit OnboardingDialog(Studio *studio, QWidget *parent = nullptr);

private:
    void detect();
    Studio *m_studio;
    QVBoxLayout *m_found;
    QLabel *m_status;
};

} // namespace lumen::ui
