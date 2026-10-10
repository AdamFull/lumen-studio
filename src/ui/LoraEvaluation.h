#pragma once

#include <QImage>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QPointer>

namespace lumen {
class Job;
}

namespace lumen::ai {
class Operation;
}

namespace lumen::ui {

class Studio;

// Tests a trained character LoRA (M13 §13.6): the newest epochs render four test shots each (poses,
// views and backgrounds that are not in the training set, prompted by the trigger word alone), next
// to a baseline without the LoRA (appearance tags and the character's reference through IP-Adapter).
// The Critic scores every image against the references. The best epoch then renders the shots at
// weights 0.6 and 1.0, and Lumen recommends an epoch and a weight. Needs a Lumen-run
// stable-diffusion.cpp server (the LoRA files are put into its LoRA folder for the test).
class LoraEvaluation : public QObject
{
    Q_OBJECT
public:
    struct Cell
    {
        int epoch = 0;          // 0 = baseline without LoRA
        double weight = 0;
        QString shot;           // test shot id
        QString image;          // saved PNG
        QJsonObject critique;
        double score = -1;
        QString error;
    };

    static LoraEvaluation *start(Studio *studio, const QString &characterId, const QString &runDir, QString *error = nullptr);
    static LoraEvaluation *running(const QString &characterId);
    // Result of an earlier test of a run (eval/eval.json); empty when there is none.
    static QJsonObject saved(const QString &runDir);

    QString characterId() const { return m_characterId; }
    QString runDir() const { return m_runDir; }
    QList<Cell> cells() const { return m_cells; }
    QList<QJsonObject> shots() const { return m_shots; }
    bool isFinished() const { return m_finished; }
    QString status() const { return m_status; }
    // Mean score of an epoch at a weight (baseline: epoch 0); -1 when not scored.
    double meanScore(int epoch, double weight) const;
    int bestEpoch() const { return m_bestEpoch; }
    double bestWeight() const { return m_bestWeight; }
    void cancel();

    // Puts the recommended (or given) epoch into the model library and attaches it to the character
    // with the weight. Returns the LoRA file, or empty on failure (*error).
    static QString apply(Studio *studio, const QString &characterId, const QString &runDir, int epoch, double weight, QString *error = nullptr);

    // The epoch files of a run: epoch number → file (the final file is the last epoch).
    static QList<QPair<int, QString>> epochFiles(const QString &runDir);

signals:
    void changed();
    void finished(bool ok, const QString &message);

private:
    LoraEvaluation(Studio *studio, const QString &characterId, const QString &runDir);
    bool begin(QString *error);
    void generateNext();
    void critiqueNext();
    void stageDone();
    void finish(bool ok, const QString &message);
    void save() const;
    QString evalLora(int epoch) const; // the test copy in the server's LoRA folder

    Studio *m_studio;
    QString m_characterId;
    QString m_runDir;
    QString m_trigger;
    QString m_providerId;
    QString m_loraDir;
    QJsonObject m_sheet;
    QList<QImage> m_references;
    QList<QJsonObject> m_shots;
    QList<QPair<int, QString>> m_epochs;   // candidates
    QList<Cell> m_cells;
    QList<int> m_pending;                  // cells to generate
    QList<int> m_toCritique;
    int m_stage = 0;                       // 1: epochs at 0.8 and baseline, 2: weights of the best epoch
    int m_bestEpoch = 0;
    double m_bestWeight = 0.8;
    bool m_finished = false;
    bool m_cancelled = false;
    QString m_status;
    QPointer<Job> m_job;
    QPointer<ai::Operation> m_critique;
};

} // namespace lumen::ui
