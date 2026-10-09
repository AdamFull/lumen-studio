#include "pose/PosePresets.h"

#include "core/Paths.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>

namespace lumen::pose {

namespace {

LimbParams limb(double raise, double direction, double bend = 0.0, double twist = 0.0)
{
    return LimbParams{raise, direction, bend, twist};
}

PosePreset make(const QString &id, const QString &name, const QString &category, const QString &description,
                const std::function<void(RigParams &)> &setup)
{
    RigParams r;
    r.name = name;
    setup(r);
    PosePreset p;
    p.id = id;
    p.name = name;
    p.category = category;
    p.description = description;
    p.rigs = {r};
    return p;
}

QList<PosePreset> buildPresets()
{
    QList<PosePreset> list;
    const QString standing = QStringLiteral("Standing");
    const QString gestures = QStringLiteral("Gestures");
    const QString action = QStringLiteral("Action");
    const QString sitting = QStringLiteral("Sitting & Ground");
    const QString views = QStringLiteral("Views");
    const QString framing = QStringLiteral("Framing");
    const QString sheets = QStringLiteral("Sheets");

    list << make(QStringLiteral("a_pose"), QStringLiteral("A-Pose (Live2D)"), standing,
                 QStringLiteral("Front view, arms angled away from the torso, legs slightly apart. Ideal base for Live2D / rigging."),
                 [](RigParams &r) {
                     r.leftArm = r.rightArm = limb(35, 90, 6);
                     r.leftLeg = r.rightLeg = limb(4, 90);
                     r.leftHand = r.rightHand = QStringLiteral("relaxed");
                     r.perspective = 0.0;
                 });
    list << make(QStringLiteral("t_pose"), QStringLiteral("T-Pose"), standing, QStringLiteral("Arms horizontal, front view."),
                 [](RigParams &r) {
                     r.leftArm = r.rightArm = limb(90, 90);
                     r.leftLeg = r.rightLeg = limb(3, 90);
                     r.leftHand = r.rightHand = QStringLiteral("open");
                     r.perspective = 0.0;
                 });
    list << make(QStringLiteral("standing"), QStringLiteral("Standing Relaxed"), standing, QStringLiteral("Natural standing pose."),
                 [](RigParams &) {});
    list << make(QStringLiteral("contrapposto"), QStringLiteral("Contrapposto"), standing,
                 QStringLiteral("Weight on one leg, relaxed asymmetry."), [](RigParams &r) {
                     r.yaw = 10;
                     r.torsoSide = -3;
                     r.headRoll = 4;
                     r.leftLeg = limb(10, 30, 18);
                     r.rightLeg = limb(2, 90);
                     r.rightArm = limb(10, 90, 15);
                     r.leftArm = limb(12, 80, 25);
                 });
    list << make(QStringLiteral("hands_on_hips"), QStringLiteral("Hands on Hips"), standing, QStringLiteral("Confident stance."),
                 [](RigParams &r) {
                     r.leftArm = r.rightArm = limb(42, 95, 115, 90);
                     r.leftLeg = r.rightLeg = limb(7, 90);
                     r.leftHand = r.rightHand = QStringLiteral("grip");
                     r.expression = QStringLiteral("smile");
                 });
    list << make(QStringLiteral("arms_crossed"), QStringLiteral("Arms Crossed"), standing, QStringLiteral("Arms folded over the chest."),
                 [](RigParams &r) {
                     r.leftArm = limb(25, 30, 120, 80);
                     r.rightArm = limb(22, 30, 125, 80);
                     r.leftHand = r.rightHand = QStringLiteral("grip");
                     r.leftLeg = r.rightLeg = limb(4, 90);
                 });
    list << make(QStringLiteral("hands_behind_back"), QStringLiteral("Hands Behind Back"), standing, QString(),
                 [](RigParams &r) {
                     r.leftArm = r.rightArm = limb(20, 160, 60, 90);
                     r.leftHand = r.rightHand = QStringLiteral("none");
                     r.headRoll = 5;
                     r.expression = QStringLiteral("smile");
                 });

    list << make(QStringLiteral("waving"), QStringLiteral("Waving"), gestures, QStringLiteral("Friendly wave with the right hand."),
                 [](RigParams &r) {
                     r.rightArm = limb(100, 90, 70, -90);
                     r.rightHand = QStringLiteral("open");
                     r.expression = QStringLiteral("smile");
                     r.headRoll = 5;
                 });
    list << make(QStringLiteral("pointing"), QStringLiteral("Pointing Forward"), gestures, QString(), [](RigParams &r) {
        r.rightArm = limb(90, 15, 0);
        r.rightHand = QStringLiteral("point");
        r.yaw = 15;
    });
    list << make(QStringLiteral("peace_sign"), QStringLiteral("Peace Sign"), gestures, QString(), [](RigParams &r) {
        r.rightArm = limb(35, 40, 135, -40);
        r.rightHand = QStringLiteral("peace");
        r.expression = QStringLiteral("smile");
        r.headRoll = 7;
    });
    list << make(QStringLiteral("thinking"), QStringLiteral("Thinking"), gestures, QStringLiteral("Hand on chin."), [](RigParams &r) {
        r.rightArm = limb(25, 10, 150, 20);
        r.rightHand = QStringLiteral("fist");
        r.leftArm = limb(20, 20, 95, 85);
        r.headRoll = 6;
        r.headPitch = -6;
        r.yaw = 12;
    });
    list << make(QStringLiteral("hand_on_heart"), QStringLiteral("Hand on Heart"), gestures, QString(), [](RigParams &r) {
        r.rightArm = limb(20, 25, 125, 80);
        r.rightHand = QStringLiteral("open");
        r.headPitch = 5;
    });
    list << make(QStringLiteral("thumbs_up"), QStringLiteral("Thumbs Up"), gestures, QString(), [](RigParams &r) {
        r.rightArm = limb(40, 30, 100, -30);
        r.rightHand = QStringLiteral("thumbs_up");
        r.expression = QStringLiteral("smile");
    });

    list << make(QStringLiteral("walking"), QStringLiteral("Walking"), action, QString(), [](RigParams &r) {
        r.yaw = 20;
        r.leftLeg = limb(20, 0, 8);
        r.rightLeg = limb(16, 180, 22);
        r.leftArm = limb(15, 180, 12);
        r.rightArm = limb(18, 0, 18);
    });
    list << make(QStringLiteral("running"), QStringLiteral("Running"), action, QString(), [](RigParams &r) {
        r.yaw = 25;
        r.torsoLean = 12;
        r.leftLeg = limb(55, 0, 80);
        r.rightLeg = limb(30, 180, 70);
        r.leftArm = limb(40, 180, 90);
        r.rightArm = limb(50, 0, 95);
        r.leftHand = r.rightHand = QStringLiteral("fist");
    });
    list << make(QStringLiteral("jumping"), QStringLiteral("Jumping for Joy"), action, QString(), [](RigParams &r) {
        r.leftArm = r.rightArm = limb(155, 75, 10);
        r.leftHand = r.rightHand = QStringLiteral("open");
        r.leftLeg = limb(15, 0, 85);
        r.rightLeg = limb(5, 60, 95);
        r.expression = QStringLiteral("open_mouth");
        r.headPitch = -10;
    });
    list << make(QStringLiteral("fighting_stance"), QStringLiteral("Fighting Stance"), action, QString(), [](RigParams &r) {
        r.yaw = 30;
        r.torsoLean = 8;
        r.headPitch = 5;
        r.leftLeg = limb(25, 20, 25);
        r.rightLeg = limb(20, 160, 20);
        r.leftArm = limb(50, 10, 100, -30);
        r.rightArm = limb(40, 20, 130, -20);
        r.leftHand = r.rightHand = QStringLiteral("fist");
        r.expression = QStringLiteral("angry");
    });
    list << make(QStringLiteral("stretching"), QStringLiteral("Stretching Up"), action, QString(), [](RigParams &r) {
        r.leftArm = r.rightArm = limb(170, 80, 15, 60);
        r.leftHand = r.rightHand = QStringLiteral("open");
        r.expression = QStringLiteral("closed_eyes");
        r.headPitch = -12;
    });

    list << make(QStringLiteral("sitting_chair"), QStringLiteral("Sitting (Chair)"), sitting, QString(), [](RigParams &r) {
        r.yaw = 25;
        r.leftLeg = limb(90, 5, 90);
        r.rightLeg = limb(88, 10, 92);
        r.leftArm = r.rightArm = limb(15, 15, 55, 30);
    });
    list << make(QStringLiteral("kneeling"), QStringLiteral("Kneeling"), sitting, QString(), [](RigParams &r) {
        r.yaw = 30;
        r.leftLeg = r.rightLeg = limb(0, 0, 95);
        r.leftArm = r.rightArm = limb(12, 20, 40, 40);
    });
    list << make(QStringLiteral("crouching"), QStringLiteral("Crouching"), sitting, QString(), [](RigParams &r) {
        r.yaw = 20;
        r.torsoLean = 35;
        r.leftLeg = limb(100, 25, 140);
        r.rightLeg = limb(95, 35, 135);
        r.leftArm = r.rightArm = limb(30, 10, 60);
    });
    list << make(QStringLiteral("sitting_floor"), QStringLiteral("Sitting on Floor"), sitting, QStringLiteral("Legs to the side."),
                 [](RigParams &r) {
                     r.yaw = 15;
                     r.leftLeg = limb(85, 40, 120, 60);
                     r.rightLeg = limb(80, 20, 130, 70);
                     r.leftArm = limb(20, 140, 10);
                     r.rightArm = limb(20, 20, 60, 40);
                 });

    list << make(QStringLiteral("three_quarter"), QStringLiteral("Three-Quarter View"), views, QString(), [](RigParams &r) {
        r.yaw = 35;
        r.headYaw = -5;
    });
    list << make(QStringLiteral("profile"), QStringLiteral("Side Profile"), views, QString(), [](RigParams &r) { r.yaw = 90; });
    list << make(QStringLiteral("back"), QStringLiteral("Back View"), views, QString(), [](RigParams &r) { r.yaw = 180; });
    list << make(QStringLiteral("looking_back"), QStringLiteral("Looking Over Shoulder"), views, QString(), [](RigParams &r) {
        r.yaw = 150;
        r.torsoTwist = -25;
        r.headYaw = -70;
    });
    list << make(QStringLiteral("low_angle"), QStringLiteral("Low Angle Hero"), views, QString(), [](RigParams &r) {
        r.cameraPitch = -25;
        r.perspective = 0.5;
        r.leftArm = r.rightArm = limb(42, 95, 115, 90);
        r.leftLeg = limb(10, 90);
        r.rightLeg = limb(10, 90);
    });
    list << make(QStringLiteral("high_angle"), QStringLiteral("High Angle"), views, QString(), [](RigParams &r) {
        r.cameraPitch = 30;
        r.perspective = 0.4;
        r.headPitch = -20;
    });

    list << make(QStringLiteral("headshot"), QStringLiteral("Headshot"), framing, QString(), [](RigParams &r) {
        r.framing = QStringLiteral("headshot");
    });
    list << make(QStringLiteral("bust"), QStringLiteral("Bust Shot"), framing, QString(), [](RigParams &r) {
        r.framing = QStringLiteral("bust");
    });
    list << make(QStringLiteral("upper_body"), QStringLiteral("Upper Body"), framing, QString(), [](RigParams &r) {
        r.framing = QStringLiteral("upper_body");
        r.leftArm = r.rightArm = limb(12, 90, 20);
    });
    list << make(QStringLiteral("cowboy_shot"), QStringLiteral("Cowboy Shot"), framing, QString(), [](RigParams &r) {
        r.framing = QStringLiteral("cowboy_shot");
        r.yaw = 15;
    });

    {
        PosePreset p;
        p.id = QStringLiteral("turnaround");
        p.name = QStringLiteral("Turnaround (4 views)");
        p.category = sheets;
        p.description = QStringLiteral("Front, three-quarter, side and back views for a character sheet.");
        const double yaws[4] = {0, 35, 90, 180};
        for (int i = 0; i < 4; ++i) {
            RigParams r;
            r.name = QStringLiteral("View %1").arg(i + 1);
            r.yaw = yaws[i];
            r.leftArm = r.rightArm = limb(12, 90, 6);
            r.positionX = 0.125 + 0.25 * i;
            r.scale = 0.95;
            r.perspective = 0.0;
            p.rigs << r;
        }
        list << p;
    }
    {
        PosePreset p;
        p.id = QStringLiteral("front_back");
        p.name = QStringLiteral("Front & Back");
        p.category = sheets;
        for (int i = 0; i < 2; ++i) {
            RigParams r;
            r.yaw = i == 0 ? 0 : 180;
            r.leftArm = r.rightArm = limb(30, 90, 6);
            r.positionX = 0.27 + 0.46 * i;
            r.scale = 0.95;
            r.perspective = 0.0;
            p.rigs << r;
        }
        list << p;
    }
    return list;
}

QString sanitizeId(const QString &name)
{
    QString id = name.toLower();
    id.replace(QRegularExpression(QStringLiteral("[^a-z0-9_-]+")), QStringLiteral("_"));
    return id.left(64);
}

} // namespace

Pose PosePreset::toPose(const QSize &canvas) const
{
    if (pose) {
        Pose p = *pose;
        p.resizeCanvas(canvas, true);
        return p;
    }
    return solvePose(rigs, canvas);
}

QJsonObject PosePreset::toJson() const
{
    QJsonObject o{{QStringLiteral("id"), id},
                  {QStringLiteral("name"), name},
                  {QStringLiteral("category"), category},
                  {QStringLiteral("description"), description}};
    if (!rigs.isEmpty()) {
        QJsonArray arr;
        for (const RigParams &r : rigs)
            arr.append(r.toJson());
        o.insert(QStringLiteral("rigs"), arr);
    }
    if (pose)
        o.insert(QStringLiteral("openpose"), pose->toOpenPoseJson());
    return o;
}

std::optional<PosePreset> PosePreset::fromJson(const QJsonObject &json)
{
    PosePreset p;
    p.id = json.value(QStringLiteral("id")).toString();
    p.name = json.value(QStringLiteral("name")).toString();
    p.category = json.value(QStringLiteral("category")).toString(QStringLiteral("User"));
    p.description = json.value(QStringLiteral("description")).toString();
    for (const QJsonValue &v : json.value(QStringLiteral("rigs")).toArray())
        p.rigs.append(RigParams::fromJson(v.toObject()));
    if (json.contains(QStringLiteral("openpose")))
        p.pose = Pose::fromOpenPoseJson(json.value(QStringLiteral("openpose")).toObject());
    if (p.name.isEmpty() || (p.rigs.isEmpty() && !p.pose))
        return std::nullopt;
    if (p.id.isEmpty())
        p.id = sanitizeId(p.name);
    p.builtIn = false;
    return p;
}

const QList<PosePreset> &builtInPresets()
{
    static const QList<PosePreset> presets = buildPresets();
    return presets;
}

std::optional<PosePreset> findBuiltInPreset(const QString &idOrName)
{
    for (const PosePreset &p : builtInPresets()) {
        if (p.id.compare(idOrName, Qt::CaseInsensitive) == 0 || p.name.compare(idOrName, Qt::CaseInsensitive) == 0)
            return p;
    }
    return std::nullopt;
}

QString userPresetDirectory()
{
    return paths::userPresets() + QStringLiteral("/poses");
}

QList<PosePreset> loadUserPresets()
{
    QList<PosePreset> list;
    QDir dir(userPresetDirectory());
    const QStringList files = dir.entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name);
    for (const QString &file : files) {
        QFile f(dir.filePath(file));
        if (!f.open(QIODevice::ReadOnly))
            continue;
        const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
        if (auto p = PosePreset::fromJson(doc.object())) {
            p->id = QFileInfo(file).completeBaseName();
            list.append(*p);
        }
    }
    return list;
}

bool saveUserPreset(PosePreset preset, QString *error)
{
    QDir().mkpath(userPresetDirectory());
    if (preset.id.isEmpty())
        preset.id = sanitizeId(preset.name);
    preset.builtIn = false;
    if (preset.category.isEmpty())
        preset.category = QStringLiteral("User");
    QSaveFile f(userPresetDirectory() + u'/' + preset.id + QStringLiteral(".json"));
    if (!f.open(QIODevice::WriteOnly)) {
        if (error)
            *error = f.errorString();
        return false;
    }
    f.write(QJsonDocument(preset.toJson()).toJson(QJsonDocument::Indented));
    if (!f.commit()) {
        if (error)
            *error = f.errorString();
        return false;
    }
    return true;
}

bool deleteUserPreset(const QString &id)
{
    return QFile::remove(userPresetDirectory() + u'/' + id + QStringLiteral(".json"));
}

} // namespace lumen::pose
