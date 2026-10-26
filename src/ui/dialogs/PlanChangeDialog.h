#pragma once

#include "ai/ChangePlan.h"

#include <QDialog>
#include <QPointer>

class QCheckBox;
class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QVBoxLayout;

namespace lumen::ai {
class LlmProvider;
class StructuredChatOperation;
}

namespace lumen::ui {

class AutoTextEdit;

// Plan this change (plan M18 §18.5): a language model that sees the picture proposes what to select,
// the request for the image model and LoRAs; every part can be changed, and Run uses what is shown.
// Nothing is selected or generated before Run.
class PlanChangeDialog : public QDialog
{
    Q_OBJECT
public:
    PlanChangeDialog(ai::LlmProvider *planner, const QString &model, const QImage &picture, const ai::PlanContext &context,
                     QWidget *parent = nullptr);
    ~PlanChangeDialog() override;

    // The plan as shown, with the user's changes.
    ai::ChangePlan plan() const;
    // The planner answered (the fields are filled) or failed.
    bool isReady() const { return m_ready; }
    // Fills the fields with a plan (the planner's answer; also for tests and screenshots).
    void showPlan(const ai::ChangePlan &plan);

signals:
    // The planner answered or failed (tests).
    void planned();

private:
    void fail(const QString &message);

    ai::PlanContext m_context;
    QPointer<ai::StructuredChatOperation> m_op;
    bool m_ready = false;
    QLabel *m_status;
    QWidget *m_fields;
    QLineEdit *m_select;
    AutoTextEdit *m_prompt;
    QWidget *m_negativeRow;
    AutoTextEdit *m_negative;
    QWidget *m_lorasRow;
    QVBoxLayout *m_loras;
    QList<QCheckBox *> m_loraBoxes;
    QLabel *m_note;
    QPushButton *m_run;
};

} // namespace lumen::ui
