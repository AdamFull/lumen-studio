#include "ui/Icons.h"

#include "ui/Theme.h"

#include <QHash>
#include <QPainter>
#include <QSvgRenderer>

namespace lumen::ui {

namespace {

// Path data in a 24×24 box, drawn with round-capped 1.8 px strokes (no fill unless "F:" prefix).
const QHash<QString, QString> &iconData()
{
    static const QHash<QString, QString> data = {
        {QStringLiteral("brush"), QStringLiteral("<path d='M18.4 3.6a2 2 0 0 1 2.8 2.8L11 16.6l-3.6-.8-.8-3.6z'/><path d='M7.4 15.8c-2.2 0-3.4 1.6-3.4 3.6 0 .5-.4 1-1 1.1 1 .9 2.4 1.5 4 1.5 2.4 0 4-1.6 4-3.8'/>")},
        {QStringLiteral("eraser"), QStringLiteral("<path d='M7 21h13'/><path d='M14.5 3.5l6 6-9.5 9.5H7l-3.5-3.5a2 2 0 0 1 0-2.8z'/><path d='M9 9l6 6'/>")},
        {QStringLiteral("move"), QStringLiteral("<path d='M12 2v20M2 12h20'/><path d='M9 5l3-3 3 3M9 19l3 3 3-3M5 9l-3 3 3 3M19 9l3 3-3 3'/>")},
        {QStringLiteral("select-rect"), QStringLiteral("<path d='M4 7V4h3M10 4h4M17 4h3v3M20 10v4M20 17v3h-3M14 20h-4M7 20H4v-3M4 14v-4'/>")},
        {QStringLiteral("select-ellipse"), QStringLiteral("<ellipse cx='12' cy='12' rx='9' ry='7' stroke-dasharray='3 2.4'/>")},
        {QStringLiteral("lasso"), QStringLiteral("<path d='M7 18.5C4.6 17.3 3 15.3 3 13c0-4.4 4-8 9-8s9 3.6 9 8-4 8-9 8c-1 0-2-.1-2.8-.4'/><circle cx='7' cy='18' r='2'/><path d='M7 20c0 1.5-.8 2-2 2'/>")},
        {QStringLiteral("mask"), QStringLiteral("<path d='M3 12c2.5-5 6-7 9-7s6.5 2 9 7c-2.5 5-6 7-9 7s-6.5-2-9-7z'/><circle cx='12' cy='12' r='3'/><path d='M4 20L20 4'/>")},
        {QStringLiteral("mask-brush"), QStringLiteral("<rect x='2.5' y='2.5' width='12' height='12' rx='2' stroke-dasharray='2.2 2'/><path d='M19.6 9.2a1.7 1.7 0 0 1 2.4 2.4l-6.9 6.9-3.1-.7-.7-3.1z'/><path d='M10.6 16.7c-1.7 0-2.6 1.2-2.6 2.8 0 .4-.3.7-.7.8.8.8 1.9 1.2 3.1 1.2 1.9 0 3.1-1.2 3.1-2.9'/>")},
        {QStringLiteral("eyedropper"), QStringLiteral("<path d='M14.5 4.5a2.8 2.8 0 0 1 4 4L16 11l1.5 1.5-1.5 1.5L10 8l1.5-1.5L13 8z'/><path d='M12 10l-7.3 7.3a2 2 0 0 0-.6 1.4V20h1.3a2 2 0 0 0 1.4-.6L14 12'/>")},
        {QStringLiteral("fill"), QStringLiteral("<path d='M5 11l7-7 8 8-7 7a2 2 0 0 1-2.8 0L5 13.8A2 2 0 0 1 5 11z'/><path d='M3 11h17'/><path d='M20.5 15.5s1.5 1.7 1.5 2.8a1.5 1.5 0 0 1-3 0c0-1.1 1.5-2.8 1.5-2.8z'/>")},
        {QStringLiteral("transform"), QStringLiteral("<rect x='5' y='5' width='14' height='14' rx='1' stroke-dasharray='3 2'/><rect x='3' y='3' width='4' height='4'/><rect x='17' y='3' width='4' height='4'/><rect x='3' y='17' width='4' height='4'/><rect x='17' y='17' width='4' height='4'/>")},
        {QStringLiteral("crop"), QStringLiteral("<path d='M6 2v14a2 2 0 0 0 2 2h14'/><path d='M18 22V8a2 2 0 0 0-2-2H2'/>")},
        {QStringLiteral("hand"), QStringLiteral("<path d='M18 11V6a2 2 0 0 0-4 0v5M14 10V4a2 2 0 0 0-4 0v6M10 10.5V6a2 2 0 0 0-4 0v8'/><path d='M18 8a2 2 0 0 1 4 0v6a8 8 0 0 1-8 8h-2c-2.8 0-4.5-.9-6-2.4l-3.6-3.6a2 2 0 0 1 2.8-2.8L6 14'/>")},
        {QStringLiteral("zoom-in"), QStringLiteral("<circle cx='11' cy='11' r='7'/><path d='M21 21l-5-5M11 8v6M8 11h6'/>")},
        {QStringLiteral("zoom-out"), QStringLiteral("<circle cx='11' cy='11' r='7'/><path d='M21 21l-5-5M8 11h6'/>")},
        {QStringLiteral("fit"), QStringLiteral("<path d='M4 9V4h5M15 4h5v5M20 15v5h-5M9 20H4v-5'/><rect x='8' y='8' width='8' height='8' rx='1'/>")},
        {QStringLiteral("undo"), QStringLiteral("<path d='M9 14L4 9l5-5'/><path d='M4 9h10.5a5.5 5.5 0 0 1 0 11H11'/>")},
        {QStringLiteral("redo"), QStringLiteral("<path d='M15 14l5-5-5-5'/><path d='M20 9H9.5a5.5 5.5 0 0 0 0 11H13'/>")},
        {QStringLiteral("layers"), QStringLiteral("<path d='M12 2l10 5-10 5L2 7z'/><path d='M2 12l10 5 10-5'/><path d='M2 17l10 5 10-5'/>")},
        {QStringLiteral("eye"), QStringLiteral("<path d='M2 12s3.5-7 10-7 10 7 10 7-3.5 7-10 7S2 12 2 12z'/><circle cx='12' cy='12' r='3'/>")},
        {QStringLiteral("eye-off"), QStringLiteral("<path d='M9.9 4.2A10 10 0 0 1 12 4c6.5 0 10 8 10 8a17 17 0 0 1-2.2 3.2M6.6 6.6C3.6 8.6 2 12 2 12s3.5 8 10 8a9.7 9.7 0 0 0 5.4-1.6'/><path d='M14.1 14.1A3 3 0 1 1 9.9 9.9'/><path d='M2 2l20 20'/>")},
        {QStringLiteral("lock"), QStringLiteral("<rect x='4' y='11' width='16' height='10' rx='2'/><path d='M8 11V7a4 4 0 0 1 8 0v4'/>")},
        {QStringLiteral("unlock"), QStringLiteral("<rect x='4' y='11' width='16' height='10' rx='2'/><path d='M8 11V7a4 4 0 0 1 7.8-1.3'/>")},
        {QStringLiteral("plus"), QStringLiteral("<path d='M12 5v14M5 12h14'/>")},
        {QStringLiteral("minus"), QStringLiteral("<path d='M5 12h14'/>")},
        {QStringLiteral("trash"), QStringLiteral("<path d='M3 6h18M8 6V4h8v2M19 6l-1 14a2 2 0 0 1-2 2H8a2 2 0 0 1-2-2L5 6'/><path d='M10 11v6M14 11v6'/>")},
        {QStringLiteral("copy"), QStringLiteral("<rect x='9' y='9' width='12' height='12' rx='2'/><path d='M5 15H4a2 2 0 0 1-2-2V4a2 2 0 0 1 2-2h9a2 2 0 0 1 2 2v1'/>")},
        {QStringLiteral("merge"), QStringLiteral("<path d='M8 3v5a4 4 0 0 0 4 4 4 4 0 0 1 4 4v5'/><path d='M16 3v5a4 4 0 0 1-4 4'/><path d='M5 18l3 3 3-3'/>")},
        {QStringLiteral("sparkles"), QStringLiteral("<path d='M12 3l1.8 4.6L18.5 9.4l-4.7 1.8L12 16l-1.8-4.8L5.5 9.4l4.7-1.8z'/><path d='M19 14l.8 2.2L22 17l-2.2.8L19 20l-.8-2.2L16 17l2.2-.8zM5 15l.6 1.4L7 17l-1.4.6L5 19l-.6-1.4L3 17l1.4-.6z'/>")},
        {QStringLiteral("wand"), QStringLiteral("<path d='M15 4V2M15 16v-2M8 9h2M20 9h2M17.8 11.8L19 13M15 9h0M17.8 6.2L19 5M3 21l9-9M12.2 6.2L11 5'/>")},
        {QStringLiteral("chat"), QStringLiteral("<path d='M21 15a2 2 0 0 1-2 2H7l-4 4V5a2 2 0 0 1 2-2h14a2 2 0 0 1 2 2z'/><path d='M8 9h8M8 13h5'/>")},
        {QStringLiteral("image"), QStringLiteral("<rect x='3' y='3' width='18' height='18' rx='2'/><circle cx='9' cy='9' r='2'/><path d='M21 15l-3.1-3.1a2 2 0 0 0-2.8 0L6 21'/>")},
        {QStringLiteral("gallery"), QStringLiteral("<rect x='3' y='3' width='7' height='7' rx='1.5'/><rect x='14' y='3' width='7' height='7' rx='1.5'/><rect x='14' y='14' width='7' height='7' rx='1.5'/><rect x='3' y='14' width='7' height='7' rx='1.5'/>")},
        {QStringLiteral("shield"), QStringLiteral("<path d='M12 2l8 3v6c0 5-3.4 9.3-8 11-4.6-1.7-8-6-8-11V5z'/>")},
        {QStringLiteral("person"), QStringLiteral("<circle cx='12' cy='4.5' r='2'/><path d='M12 7v7M12 9l-5 4M12 9l5-2M12 14l-3.5 7M12 14l4 6'/>")},
        {QStringLiteral("settings"), QStringLiteral("<circle cx='12' cy='12' r='3'/><path d='M19.4 15a1.6 1.6 0 0 0 .3 1.8l.1.1a2 2 0 1 1-2.8 2.8l-.1-.1a1.6 1.6 0 0 0-1.8-.3 1.6 1.6 0 0 0-1 1.5V21a2 2 0 0 1-4 0v-.1a1.6 1.6 0 0 0-1-1.5 1.6 1.6 0 0 0-1.8.3l-.1.1a2 2 0 1 1-2.8-2.8l.1-.1a1.6 1.6 0 0 0 .3-1.8 1.6 1.6 0 0 0-1.5-1H3a2 2 0 0 1 0-4h.1a1.6 1.6 0 0 0 1.5-1 1.6 1.6 0 0 0-.3-1.8l-.1-.1a2 2 0 1 1 2.8-2.8l.1.1a1.6 1.6 0 0 0 1.8.3H9a1.6 1.6 0 0 0 1-1.5V3a2 2 0 0 1 4 0v.1a1.6 1.6 0 0 0 1 1.5 1.6 1.6 0 0 0 1.8-.3l.1-.1a2 2 0 1 1 2.8 2.8l-.1.1a1.6 1.6 0 0 0-.3 1.8V9a1.6 1.6 0 0 0 1.5 1H21a2 2 0 0 1 0 4h-.1a1.6 1.6 0 0 0-1.5 1z'/>")},
        {QStringLiteral("play"), QStringLiteral("<path d='M6 4l14 8-14 8z'/>")},
        {QStringLiteral("stop"), QStringLiteral("<rect x='5' y='5' width='14' height='14' rx='2'/>")},
        {QStringLiteral("refresh"), QStringLiteral("<path d='M21 12a9 9 0 0 1-15.5 6.2L3 16'/><path d='M3 12a9 9 0 0 1 15.5-6.2L21 8'/><path d='M21 3v5h-5M3 21v-5h5'/>")},
        {QStringLiteral("upload"), QStringLiteral("<path d='M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4'/><path d='M17 8l-5-5-5 5M12 3v12'/>")},
        {QStringLiteral("download"), QStringLiteral("<path d='M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4'/><path d='M7 10l5 5 5-5M12 15V3'/>")},
        {QStringLiteral("folder"), QStringLiteral("<path d='M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z'/>")},
        {QStringLiteral("save"), QStringLiteral("<path d='M19 21H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h11l5 5v11a2 2 0 0 1-2 2z'/><path d='M17 21v-8H7v8M7 3v5h8'/>")},
        {QStringLiteral("close"), QStringLiteral("<path d='M18 6L6 18M6 6l12 12'/>")},
        {QStringLiteral("check"), QStringLiteral("<path d='M20 6L9 17l-5-5'/>")},
        {QStringLiteral("chevron-down"), QStringLiteral("<path d='M6 9l6 6 6-6'/>")},
        {QStringLiteral("chevron-up"), QStringLiteral("<path d='M6 15l6-6 6 6'/>")},
        {QStringLiteral("chevron-left"), QStringLiteral("<path d='M15 6l-6 6 6 6'/>")},
        {QStringLiteral("chevron-right"), QStringLiteral("<path d='M9 6l6 6-6 6'/>")},
        {QStringLiteral("send"), QStringLiteral("<path d='M22 2L11 13'/><path d='M22 2l-7 20-4-9-9-4z'/>")},
        {QStringLiteral("paperclip"), QStringLiteral("<path d='M21.4 11.1l-9.2 9.2a6 6 0 0 1-8.5-8.5l9.2-9.2a4 4 0 0 1 5.7 5.7l-9.2 9.2a2 2 0 0 1-2.8-2.8l8.5-8.5'/>")},
        {QStringLiteral("star"), QStringLiteral("<path d='M12 2l3.1 6.3 6.9 1-5 4.9 1.2 6.8L12 17.8 5.8 21l1.2-6.8-5-4.9 6.9-1z'/>")},
        {QStringLiteral("star-filled"), QStringLiteral("F:<path d='M12 2l3.1 6.3 6.9 1-5 4.9 1.2 6.8L12 17.8 5.8 21l1.2-6.8-5-4.9 6.9-1z'/>")},
        {QStringLiteral("upscale"), QStringLiteral("<path d='M15 3h6v6M9 21H3v-6M21 3l-7 7M3 21l7-7'/>")},
        {QStringLiteral("scissors"), QStringLiteral("<circle cx='6' cy='6' r='3'/><circle cx='6' cy='18' r='3'/><path d='M20 4L8.1 15.9M14.5 14.5L20 20M8.1 8.1L12 12'/>")},
        {QStringLiteral("history"), QStringLiteral("<path d='M3 12a9 9 0 1 0 3-6.7L3 8'/><path d='M3 3v5h5M12 7v5l4 2'/>")},
        {QStringLiteral("queue"), QStringLiteral("<path d='M8 6h13M8 12h13M8 18h13'/><circle cx='4' cy='6' r='1'/><circle cx='4' cy='12' r='1'/><circle cx='4' cy='18' r='1'/>")},
        {QStringLiteral("info"), QStringLiteral("<circle cx='12' cy='12' r='9'/><path d='M12 16v-4M12 8h0'/>")},
        {QStringLiteral("warning"), QStringLiteral("<path d='M10.3 3.9L1.8 18a2 2 0 0 0 1.7 3h17a2 2 0 0 0 1.7-3L13.7 3.9a2 2 0 0 0-3.4 0z'/><path d='M12 9v4M12 17h0'/>")},
        {QStringLiteral("server"), QStringLiteral("<rect x='2' y='3' width='20' height='8' rx='2'/><rect x='2' y='13' width='20' height='8' rx='2'/><path d='M6 7h0M6 17h0'/>")},
        {QStringLiteral("cloud"), QStringLiteral("<path d='M17.5 19H8a6 6 0 1 1 1.3-11.9A7 7 0 0 1 22 12.5 4.5 4.5 0 0 1 17.5 19z'/>")},
        {QStringLiteral("sun"), QStringLiteral("<circle cx='12' cy='12' r='4'/><path d='M12 2v2M12 20v2M4.9 4.9l1.4 1.4M17.7 17.7l1.4 1.4M2 12h2M20 12h2M4.9 19.1l1.4-1.4M17.7 6.3l1.4-1.4'/>")},
        {QStringLiteral("moon"), QStringLiteral("<path d='M21 12.8A9 9 0 1 1 11.2 3a7 7 0 0 0 9.8 9.8z'/>")},
        {QStringLiteral("flip"), QStringLiteral("<path d='M12 3v18'/><path d='M8 7L3 12l5 5zM16 7l5 5-5 5z'/>")},
        {QStringLiteral("edit"), QStringLiteral("<path d='M12 20h9'/><path d='M16.5 3.5a2.1 2.1 0 0 1 3 3L7 19l-4 1 1-4z'/>")},
        {QStringLiteral("palette"), QStringLiteral("<path d='M12 22a10 10 0 1 1 10-10c0 2.8-2.2 4-4 4h-2a2 2 0 0 0-1.5 3.3A1.7 1.7 0 0 1 12 22z'/><circle cx='7.5' cy='10.5' r='1'/><circle cx='12' cy='7' r='1'/><circle cx='16.5' cy='10.5' r='1'/>")},
        {QStringLiteral("expand"), QStringLiteral("<path d='M3 3h7v7H3zM14 14h7v7h-7z'/><path d='M14 3h7v7M3 14v7h7'/>")},
        {QStringLiteral("target"), QStringLiteral("<circle cx='12' cy='12' r='9'/><circle cx='12' cy='12' r='5'/><circle cx='12' cy='12' r='1'/>")},
        {QStringLiteral("link"), QStringLiteral("<path d='M10 13a5 5 0 0 0 7.5.5l3-3a5 5 0 0 0-7-7l-1.7 1.7'/><path d='M14 11a5 5 0 0 0-7.5-.5l-3 3a5 5 0 0 0 7 7l1.7-1.7'/>")},
        {QStringLiteral("dots"), QStringLiteral("<circle cx='5' cy='12' r='1'/><circle cx='12' cy='12' r='1'/><circle cx='19' cy='12' r='1'/>")},
        {QStringLiteral("text"), QStringLiteral("<path d='M4 7V4h16v3M9 20h6M12 4v16'/>")},
        {QStringLiteral("adjust"), QStringLiteral("<path d='M4 21v-7M4 10V3M12 21v-9M12 8V3M20 21v-5M20 12V3M1 14h6M9 8h6M17 16h6'/>")},
        {QStringLiteral("outpaint"), QStringLiteral("<rect x='7' y='7' width='10' height='10' rx='1'/><path d='M3 3h3M3 3v3M21 3h-3M21 3v3M3 21h3M3 21v-3M21 21h-3M21 21v-3'/>")},
        {QStringLiteral("variation"), QStringLiteral("<rect x='3' y='3' width='8' height='8' rx='1.5'/><path d='M14 7h4a3 3 0 0 1 3 3v4'/><path d='M18 18h-4a3 3 0 0 1-3-3v-1'/><rect x='13' y='13' width='8' height='8' rx='1.5'/>")},
        {QStringLiteral("parts"), QStringLiteral("<circle cx='12' cy='4.8' r='2.6'/><path d='M8.6 10h6.8l.9 5.6H7.7z'/><path d='M5.2 9.6l2 4.4M18.8 9.6l-2 4.4'/><path d='M10 18.4l-.8 3.6M14 18.4l.8 3.6'/>")},
        {QStringLiteral("split"), QStringLiteral("<rect x='3' y='5' width='7.5' height='14' rx='2'/><rect x='13.5' y='5' width='7.5' height='14' rx='2'/><path d='M12 2v20' stroke-dasharray='2 2'/>")},
        {QStringLiteral("logo"), QStringLiteral("<path d='M12 2l2.6 6.4L21 11l-6.4 2.6L12 20l-2.6-6.4L3 11l6.4-2.6z'/><circle cx='12' cy='11' r='2'/>")},
    };
    return data;
}

QHash<QString, QIcon> &cache()
{
    static QHash<QString, QIcon> c;
    return c;
}

} // namespace

QPixmap Icons::pixmap(const QString &name, int size, const QColor &color, qreal dpr)
{
    QString body = iconData().value(name, iconData().value(QStringLiteral("dots")));
    const bool filled = body.startsWith(QLatin1String("F:"));
    if (filled)
        body = body.mid(2);
    const QString svg = QStringLiteral("<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='%1' stroke='%2' "
                                       "stroke-width='1.8' stroke-linecap='round' stroke-linejoin='round'>%3</svg>")
                            .arg(filled ? color.name() : QStringLiteral("none"), color.name(), body);
    QSvgRenderer renderer(svg.toUtf8());
    QPixmap pm(QSize(size, size) * dpr);
    pm.fill(Qt::transparent);
    pm.setDevicePixelRatio(dpr);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    if (color.alpha() < 255)
        p.setOpacity(color.alphaF());
    renderer.render(&p, QRectF(0, 0, size, size));
    p.end();
    return pm;
}

QIcon Icons::get(const QString &name)
{
    auto &c = cache();
    auto it = c.find(name);
    if (it != c.end())
        return *it;
    const Palette &pal = Theme::palette();
    QIcon icon;
    for (int size : {16, 20, 24, 32}) {
        icon.addPixmap(pixmap(name, size, pal.icon), QIcon::Normal, QIcon::Off);
        icon.addPixmap(pixmap(name, size, pal.accent), QIcon::Normal, QIcon::On);
        icon.addPixmap(pixmap(name, size, pal.textMuted.darker(130)), QIcon::Disabled, QIcon::Off);
        icon.addPixmap(pixmap(name, size, pal.text), QIcon::Active, QIcon::Off);
        icon.addPixmap(pixmap(name, size, pal.accent), QIcon::Active, QIcon::On);
        icon.addPixmap(pixmap(name, size, pal.accent), QIcon::Selected, QIcon::Off);
    }
    c.insert(name, icon);
    return icon;
}

void Icons::clearCache()
{
    cache().clear();
}

QStringList Icons::names()
{
    return iconData().keys();
}

} // namespace lumen::ui
