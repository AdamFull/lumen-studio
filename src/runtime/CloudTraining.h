#pragma once

#include "runtime/LoraTraining.h"
#include "runtime/RunPod.h"

#include <functional>

namespace lumen {
class Job;
class JobQueue;
}

namespace lumen::net {
class HttpClient;
}

namespace lumen::runtime {

// A LoRA training on a rented RunPod GPU (M15).
struct CloudTrainingOptions
{
    // What to train: as for a local training. `checkpoint` and `imagesDir` are local; the LoRA
    // files and train.log go to `workDir` here. `runtimeId` and `resumeState` are not used.
    TrainingOptions training;
    QString gpuId;                 // RunPod GPU type
    bool community = false;
    QString image = QStringLiteral("runpod/pytorch:2.8.0-py3.11-cuda12.8.1-cudnn-devel-ubuntu22.04");
    int diskGb = 80;
    // When set, the pod downloads the base model (checked against the SHA-256) instead of
    // receiving it from this computer.
    QString checkpointUrl;
    QString checkpointSha256;
    // Tests: a script uploaded and run instead of sd-scripts (setup skipped), how fast to poll,
    // and where the agent of a pod is (default: RunPod's proxy).
    QString testTrainer;
    int pollMs = 5000;
    int startTimeoutSec = 900;
    std::function<QString(const RunPodPod &)> agentUrl;
};

// Runs a cloud training as a Queue job (group "cloud-training"). The pod is terminated when the
// job ends in any way. The job result is a map like a local training's: {"files", "final", "log"},
// plus "pod" and "costPerHour". Metadata while it runs: "pod", "costPerHour", "stage".
Job *startCloudTraining(RunPodClient *runPod, net::HttpClient *http, JobQueue *jobs, const CloudTrainingOptions &options);

// Pods this Lumen is training on right now (other pods with Lumen's name prefix are leftovers).
QStringList activeCloudPods();
// Lumen's pods that still exist but no training here uses: left by a crash or a failed shutdown.
// They cost money until terminated.
QFuture<QList<RunPodPod>> leftoverCloudPods(RunPodClient *runPod);

} // namespace lumen::runtime
