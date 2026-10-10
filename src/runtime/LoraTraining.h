#pragma once

#include "runtime/RuntimeStore.h"
#include "runtime/ServerProfile.h"

#include <QString>
#include <QStringList>
#include <QVariantMap>

namespace lumen {
class Job;
class JobQueue;
}

namespace lumen::runtime {

class ServerSupervisor;

// Training a character LoRA with kohya sd-scripts (M13 §13.6).
struct TrainingOptions
{
    QString runtimeId;        // installed trainer (kind "kohya")
    QString checkpoint;       // base model file the LoRA is trained on
    QString family;           // "SDXL" (also Illustrious / Pony) or "SD 1.5"
    QString imagesDir;        // folder that holds "<repeats>_<trigger>" with images and captions
    QString workDir;          // run folder: output, state, log
    QString outputName;       // LoRA file name stem
    QString trigger;
    int epochs = 10;
    VramTier tier = VramTier::Small;
    int seed = 42;
    QString resumeState;      // state folder to continue from (empty = new run)
    QString title;            // for the file's metadata
};

// Settings per GPU size; 8 GB is the lower limit for SDXL (U-Net only, cached text-encoder
// outputs, Adafactor, gradient checkpointing, bf16).
struct TrainingPreset
{
    int dim = 16;
    int alpha = 8;
    bool unetOnly = true;
    QString optimizer;        // "Adafactor", "AdamW8bit"
    QStringList optimizerArgs;
    double unetLr = 1e-4;
    double textLr = 0;        // with text encoders
    int batch = 1;
    bool fp8Base = false;     // base model weights in fp8 (less memory, a little less quality)
};
TrainingPreset trainingPreset(VramTier tier);

// Repeats per image so that one epoch is about `stepsPerEpoch` steps (kohya multiplies images by
// the "<repeats>_" folder prefix).
int repeatsFor(int images, int stepsPerEpoch = 200);
int totalSteps(int images, int repeats, int epochs, int batch = 1);

// Arguments after the Python executable: the training script and its options.
QStringList trainingArguments(const TrainingOptions &options, const InstalledRuntime &trainer);

struct TrainingProgress
{
    QString phase;            // "caching", "training", "saving"
    int step = -1;
    int totalSteps = -1;
    int epoch = -1;
    int epochs = -1;
    double loss = -1;
    QString remaining;        // "14:40" (tqdm's estimate)
    double secondsPerStep = -1;
    QString savedFile;        // a LoRA written just now
    QString error;            // a known failure ("out of GPU memory")
};
// Updates `p` from one line of the trainer's output; returns whether anything changed.
bool parseTrainingLine(const QString &line, TrainingProgress *p);

// The LoRA files of a run: one per epoch ("<name>-000003.safetensors") and the final one.
QStringList trainingOutputs(const QString &workDir, const QString &outputName);
// The newest saved state ("<name>-000004-state"), for resuming.
QString latestTrainingState(const QString &workDir);

// Runs a training as a Queue job (group "lora-training", one at a time). The GPU is reserved:
// running servers stop and come back afterwards. The job result is a map: {"files": [epoch files
// and the final file], "final": path, "log": path}.
Job *startTraining(RuntimeStore *store, ServerSupervisor *servers, JobQueue *jobs, const TrainingOptions &options);

} // namespace lumen::runtime
