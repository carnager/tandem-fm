#include "ui/PathBar.h"

#include "core/Backend.h"
#include "core/Remote.h"

#include "core/Util.h"

#include <QDir>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QResizeEvent>
#include <QMenu>
#include <QStackedLayout>
#include <QToolButton>

namespace tandem {

namespace {
struct Segment {
    QString label;
    QString path;
};

QList<Segment> segmentsOf(const QString& path)
{
    QList<Segment> out;
    QString rest;
    if (isRemoteLocation(path)) {  // sftp://bob@host/srv → "⇄ sftp://bob@host" › srv
        const QString root = locationRoot(path);
        out.append({QStringLiteral("⇄ ") + locationAuthority(path), root});
        rest = locationPath(path);
        QString acc = root;
        for (const QString& part : rest.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
            acc = joinPath(acc, part);
            out.append({part, acc});
        }
        return out;
    } else if (const auto r = remoteRootOf(path)) {
        out.append({QStringLiteral("⇄ ") + r->label, r->root});  // the GVfs folder name is unreadable
        rest = path.mid(r->root.size());
    } else {
        out.append({QStringLiteral("/"), QStringLiteral("/")});
        rest = path;
    }
    QString acc = out.first().path;
    for (const QString& part : rest.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
        acc = joinPath(acc, part);
        out.append({part, acc});
    }
    return out;
}
}  // namespace

PathBar::PathBar(QWidget* parent) : QWidget(parent)
{
    m_stack = new QStackedLayout(this);
    m_stack->setContentsMargins(0, 0, 0, 0);
    m_crumbs = new QWidget(this);
    m_crumbLayout = new QHBoxLayout(m_crumbs);
    m_crumbLayout->setContentsMargins(0, 0, 0, 0);
    m_crumbLayout->setSpacing(0);
    m_edit = new QLineEdit(this);
    m_edit->setFrame(false);
    m_edit->installEventFilter(this);
    m_stack->addWidget(m_crumbs);
    m_stack->addWidget(m_edit);
    // The segments shrink into "…" to fit, so the bar must never demand
    // their full width (that would stop the splitter from resizing panels).
    m_crumbs->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_edit->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    setCursor(Qt::IBeamCursor);
    connect(m_edit, &QLineEdit::returnPressed, this, [this] {
        const QString text = m_edit->text();
        stopEditing();
        emit pathEntered(text);
    });
}

bool PathBar::isEditing() const
{
    return m_stack->currentWidget() == m_edit;
}

void PathBar::setPath(const QString& path)
{
    if (path == m_path)
        return;
    m_path = path;
    if (!isEditing())
        scheduleRebuild();
}

// Rebuilding deletes and creates child widgets; never do that in the middle
// of a resize/layout pass, only from the event loop.
void PathBar::scheduleRebuild()
{
    if (m_rebuildPending)
        return;
    m_rebuildPending = true;
    QMetaObject::invokeMethod(this, [this] {
        m_rebuildPending = false;
        if (!isEditing())
            rebuild();
    }, Qt::QueuedConnection);
}

void PathBar::startEditing()
{
    m_edit->setText(m_path);
    m_stack->setCurrentWidget(m_edit);
    m_edit->setFocus();
    m_edit->selectAll();
}

void PathBar::stopEditing()
{
    m_stack->setCurrentWidget(m_crumbs);
    scheduleRebuild();
}

void PathBar::resizeEvent(QResizeEvent* e)
{
    QWidget::resizeEvent(e);
    if (!isEditing() && e->size().width() != e->oldSize().width())
        scheduleRebuild();
}

void PathBar::mousePressEvent(QMouseEvent* e)
{
    // Clicks between/after the segments start typing.
    if (e->button() == Qt::LeftButton)
        startEditing();
}

bool PathBar::eventFilter(QObject* obj, QEvent* e)
{
    if (obj == m_edit) {
        if (e->type() == QEvent::FocusIn)
            emit focused();
        if (e->type() == QEvent::FocusOut && isEditing())
            stopEditing();
        if (e->type() == QEvent::KeyPress && static_cast<QKeyEvent*>(e)->key() == Qt::Key_Escape) {
            stopEditing();
            return false;  // let the panel move focus back to the list
        }
        return false;
    }
    if (e->type() == QEvent::MouseButtonRelease && static_cast<QMouseEvent*>(e)->button() == Qt::MiddleButton) {
        const QString target = obj->property("path").toString();
        if (!target.isEmpty())
            emit segmentMiddleClicked(target);
        return true;
    }
    return false;
}

void PathBar::rebuild()
{
    while (QLayoutItem* item = m_crumbLayout->takeAt(0)) {
        if (QWidget* w = item->widget()) {
            w->hide();
            w->deleteLater();  // never delete widgets synchronously during a rebuild
        }
        delete item;
    }
    if (m_path.isEmpty())
        return;
    const QList<Segment> segs = segmentsOf(m_path);

    auto makeButton = [this](const Segment& s, bool last) {
        auto* b = new QToolButton(m_crumbs);
        b->setObjectName(QStringLiteral("crumb"));
        b->setText(s.label);
        b->setAutoRaise(true);
        b->setFocusPolicy(Qt::NoFocus);
        b->setCursor(Qt::PointingHandCursor);
        b->setToolTip(s.path);
        b->setProperty("path", s.path);
        if (last) {
            QFont f = b->font();
            f.setWeight(QFont::DemiBold);
            b->setFont(f);
        }
        b->installEventFilter(this);
        connect(b, &QToolButton::clicked, this, [this, p = s.path] { emit segmentClicked(p); });
        return b;
    };
    auto makeSeparator = [this] {
        auto* l = new QLabel(QStringLiteral("›"), m_crumbs);
        l->setObjectName(QStringLiteral("crumbSep"));
        l->setContentsMargins(2, 0, 2, 0);
        return l;
    };

    // Fit from the end; what doesn't fit goes into a "…" menu at the front.
    const int available = qMax(60, width() - 4);
    QList<QWidget*> kept;
    int used = 0;
    int first = int(segs.size());
    for (int i = int(segs.size()) - 1; i >= 0; --i) {
        QToolButton* b = makeButton(segs[i], i == segs.size() - 1);
        const int w = b->sizeHint().width() + (i > 0 ? 14 : 0);
        if (used + w > available && i < segs.size() - 1) {
            b->deleteLater();
            break;
        }
        used += w;
        kept.prepend(b);
        first = i;
    }
    // Shown before going into the layout: otherwise Qt queues a deferred show
    // for each new child, which misbehaves when the bar is rebuilt quickly.
    auto add = [this](QWidget* w) {
        w->show();
        m_crumbLayout->addWidget(w);
    };
    if (first > 0) {
        auto* more = new QToolButton(m_crumbs);
        more->setObjectName(QStringLiteral("crumb"));
        more->setText(QStringLiteral("…"));
        more->setAutoRaise(true);
        more->setFocusPolicy(Qt::NoFocus);
        more->setPopupMode(QToolButton::InstantPopup);
        auto* menu = new QMenu(more);
        for (int i = first - 1; i >= 0; --i)
            menu->addAction(segs[i].path, this, [this, p = segs[i].path] { emit segmentClicked(p); });
        more->setMenu(menu);
        add(more);
        add(makeSeparator());
    }
    for (int i = 0; i < kept.size(); ++i) {
        if (i > 0)
            add(makeSeparator());
        add(kept[i]);
    }
    m_crumbLayout->addStretch(1);
}

}  // namespace tandem
