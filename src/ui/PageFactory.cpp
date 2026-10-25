#include "ui/PageFactory.h"

#include "ui/Studio.h"
#include "ui/pages/EditorPage.h"
#include "ui/pages/GeneratePage.h"
#include "ui/pages/LibraryPage.h"

#include <QCoreApplication>

namespace lumen::ui {

// The rail labels keep the translation context they had in MainWindow.
QList<ext::PageSpec> corePages()
{
    ext::PageSpec generate;
    generate.id = pages::Generate;
    generate.icon = QStringLiteral("sparkles");
    generate.label = QCoreApplication::translate("lumen::ui::MainWindow", "Generate");
    generate.tooltip = QCoreApplication::translate("lumen::ui::MainWindow", "Text/image to image generation");
    generate.order = 20;
    generate.shortcut = QKeySequence(QStringLiteral("Alt+2"));
    generate.create = [](Studio *studio, QWidget *parent) -> QWidget * { return new GeneratePage(studio, parent); };

    ext::PageSpec editor;
    editor.id = pages::Editor;
    editor.icon = QStringLiteral("brush");
    editor.label = QCoreApplication::translate("lumen::ui::MainWindow", "Edit");
    editor.tooltip = QCoreApplication::translate("lumen::ui::MainWindow", "Layered editor with inpainting and AI tools");
    editor.order = 30;
    editor.shortcut = QKeySequence(QStringLiteral("Alt+3"));
    editor.create = [](Studio *studio, QWidget *parent) -> QWidget * { return new EditorPage(studio, parent); };

    ext::PageSpec library;
    library.id = pages::Library;
    library.icon = QStringLiteral("gallery");
    library.label = QCoreApplication::translate("lumen::ui::MainWindow", "Library");
    library.tooltip = QCoreApplication::translate("lumen::ui::MainWindow", "All images of the project");
    library.order = 60;
    library.shortcut = QKeySequence(QStringLiteral("Alt+6"));
    library.create = [](Studio *studio, QWidget *parent) -> QWidget * { return new LibraryPage(studio, parent); };

    return {generate, editor, library};
}

} // namespace lumen::ui
