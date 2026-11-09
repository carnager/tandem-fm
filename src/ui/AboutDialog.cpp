#include "ui/AboutDialog.h"

#include "ui/Theme.h"

#include <QApplication>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QVBoxLayout>

namespace tandem {

namespace {

// The mascot, scaled for the screen, with rounded corners.
QPixmap mascot(int width, qreal dpr)
{
    const QImage owl(QStringLiteral(":/brand/tandem-owl.jpg"));
    if (owl.isNull())
        return {};
    const QSize size(width, width * owl.height() / owl.width());
    QPixmap out(size * dpr);
    out.setDevicePixelRatio(dpr);
    out.fill(Qt::transparent);
    QPainter p(&out);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    QPainterPath clip;
    clip.addRoundedRect(QRectF(QPointF(0, 0), size), 12, 12);
    p.setClipPath(clip);
    p.drawImage(QRectF(QPointF(0, 0), size), owl);
    return out;
}

}  // namespace

AboutDialog::AboutDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(tr("About Tandem"));

    auto* picture = new QLabel(this);
    picture->setObjectName(QStringLiteral("mascot"));
    picture->setPixmap(mascot(qRound(180 * interfaceScale()), devicePixelRatioF()));

    auto* name = new QLabel(QStringLiteral("Tandem"), this);
    QFont big = name->font();
    big.setPointSizeF(big.pointSizeF() * 2.2);
    big.setWeight(QFont::DemiBold);
    name->setFont(big);

    auto* what = new QLabel(tr("Two-panel file manager"), this);
    setThemeStyle(what, QStringLiteral("color: @dimText;"));

    auto* version = new QLabel(tr("Version %1").arg(QApplication::applicationVersion()), this);
    version->setTextInteractionFlags(Qt::TextSelectableByMouse);

    const QString site = QStringLiteral("https://github.com/carnager/tandem-fm");
    auto* about = new QLabel(this);
    about->setWordWrap(true);
    about->setTextFormat(Qt::RichText);
    about->setOpenExternalLinks(true);
    about->setText(tr("Free software under the GNU General Public License, version 3 or later.")
                   + QStringLiteral("<br><br><a href=\"%1\">%2</a>").arg(site, QStringLiteral("github.com/carnager/tandem-fm")));
    auto* qt = new QLabel(tr("Built with Qt %1").arg(QString::fromLatin1(qVersion())), this);
    setThemeStyle(qt, QStringLiteral("color: @dimText;"));

    auto* text = new QVBoxLayout;
    text->setSpacing(4);
    text->addStretch();
    text->addWidget(name);
    text->addWidget(what);
    text->addSpacing(12);
    text->addWidget(version);
    text->addWidget(qt);
    text->addSpacing(12);
    text->addWidget(about);
    text->addStretch();

    auto* row = new QHBoxLayout;
    row->setSpacing(24);
    row->addWidget(picture, 0, Qt::AlignTop);
    row->addLayout(text, 1);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 20, 20, 16);
    layout->addLayout(row);
    layout->addWidget(buttons);
    setMinimumWidth(qRound(520 * interfaceScale()));
}

}  // namespace tandem
