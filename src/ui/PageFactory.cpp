#include "ui/PageFactory.h"

#include "ui/pages/CreatePage.h"
#include "ui/pages/EditorPage.h"
#include "ui/pages/GeneratePage.h"
#include "ui/pages/LibraryPage.h"
#include "ui/parts/PartsPage.h"
#include "ui/pages/PosePage.h"

namespace lumen::ui {

QWidget *createPage(Page page, Studio *studio, QWidget *parent)
{
    switch (page) {
    case Page::Create: return new CreatePage(studio, parent);
    case Page::Generate: return new GeneratePage(studio, parent);
    case Page::Editor: return new EditorPage(studio, parent);
    case Page::Poses: return new PosePage(studio, parent);
    case Page::Library: return new LibraryPage(studio, parent);
    case Page::Parts: return new PartsPage(studio, parent);
    }
    return new QWidget(parent);
}

} // namespace lumen::ui
