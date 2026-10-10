#pragma once

#include <QWidget>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;

namespace lumen::ui {

class Studio;

// Settings → Cloud training (M15): the RunPod API key (kept in the credential store), Secure or
// Community cloud, and the preferred GPU from RunPod's catalogue (24 GB or more).
class CloudTrainingPage : public QWidget
{
    Q_OBJECT
public:
    explicit CloudTrainingPage(Studio *studio, QWidget *parent = nullptr);

    // Fills the GPU list from the catalogue (also done when the page opens with a key).
    void refreshGpus();

private:
    void saveKey();
    void forgetKey();
    void updateKeyState();

    Studio *m_studio;
    QLineEdit *m_key;
    QPushButton *m_saveKey;
    QPushButton *m_forgetKey;
    QComboBox *m_cloud;
    QComboBox *m_gpu;
    QLabel *m_status;
};

} // namespace lumen::ui
