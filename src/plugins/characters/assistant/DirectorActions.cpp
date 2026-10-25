#include "plugins/characters/assistant/DirectorActions.h"

#include "ui/Studio.h"

#include <QHash>
#include <QPointer>

namespace lumen::ui {

namespace {

struct Registration
{
    DirectorActions *actions = nullptr;
    QPointer<QObject> owner;
};

QHash<const Studio *, Registration> &registrations()
{
    static QHash<const Studio *, Registration> map;
    return map;
}

} // namespace

DirectorActions *directorActions(const Studio *studio)
{
    const Registration r = registrations().value(studio);
    return r.owner ? r.actions : nullptr;
}

void setDirectorActions(Studio *studio, DirectorActions *actions, QObject *owner)
{
    // A studio that goes away takes its entry along (a later studio may get the same address).
    if (!registrations().contains(studio))
        QObject::connect(studio, &QObject::destroyed, [studio] { registrations().remove(studio); });
    registrations().insert(studio, Registration{actions, owner});
}

} // namespace lumen::ui
