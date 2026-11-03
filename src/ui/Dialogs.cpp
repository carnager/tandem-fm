#include "ui/Dialogs.h"

#include "core/Util.h"
#include "ui/Theme.h"

#include <QDateTime>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QShortcut>
#include <QVBoxLayout>

namespace tandem {

QString askDestination(QWidget* parent, const QString& title, const QString& label, const QString& initial,
                       bool* startNow)
{
    QDialog dlg(parent);
    dlg.setWindowTitle(title);
    dlg.setMinimumWidth(560);
    auto* layout = new QVBoxLayout(&dlg);
    layout->setContentsMargins(18, 16, 18, 14);
    layout->setSpacing(10);
    auto* lbl = new QLabel(label, &dlg);
    lbl->setWordWrap(true);
    auto* edit = new QLineEdit(initial, &dlg);
    edit->selectAll();
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    layout->addWidget(lbl);
    layout->addWidget(edit);
    if (startNow) {
        *startNow = false;
        auto* hint = new QLabel(QObject::tr("Enter: queue behind other operations on the same drive   ·   "
                                            "Shift+Enter: start right away"),
                                &dlg);
        setThemeStyle(hint, QStringLiteral("color: @dimText;"));
        layout->addWidget(hint);
        for (auto key : {Qt::SHIFT | Qt::Key_Return, Qt::SHIFT | Qt::Key_Enter}) {
            auto* sc = new QShortcut(QKeySequence(key), &dlg);
            QObject::connect(sc, &QShortcut::activated, &dlg, [&dlg, startNow] {
                *startNow = true;
                dlg.accept();
            });
        }
    }
    layout->addWidget(buttons);
    if (dlg.exec() != QDialog::Accepted)
        return {};
    return edit->text().trimmed();
}

static QString describe(int64_t size, int64_t time)
{
    const QString when = QDateTime::fromSecsSinceEpoch(time).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    if (size < 0)
        return QObject::tr("folder, modified %1").arg(when);
    return QObject::tr("%1, modified %2").arg(formatSize(size), when);
}

PromptDialog::PromptDialog(const PromptRequest& req, QWidget* parent) : QDialog(parent)
{
    setWindowTitle(req.jobTitle);
    setMinimumWidth(520);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(18, 16, 18, 14);
    layout->setSpacing(10);

    auto* title = new QLabel(req.message, this);
    QFont f = title->font();
    f.setPointSizeF(f.pointSizeF() * 1.1);
    f.setWeight(QFont::DemiBold);
    title->setFont(f);
    title->setWordWrap(true);
    layout->addWidget(title);

    auto addPathRow = [&](QGridLayout* g, int row, const QString& caption, const QString& path, const QString& info) {
        auto* c = new QLabel(caption, this);
        setThemeStyle(c, QStringLiteral("color: @dimText;"));
        auto* p = new QLabel(path, this);
        p->setTextInteractionFlags(Qt::TextSelectableByMouse);
        p->setWordWrap(true);
        g->addWidget(c, row, 0, Qt::AlignTop);
        g->addWidget(p, row, 1);
        if (!info.isEmpty()) {
            auto* i = new QLabel(info, this);
            setThemeStyle(i, QStringLiteral("color: @dimText;"));
            g->addWidget(i, row + 1, 1);
        }
    };

    auto* grid = new QGridLayout;
    grid->setHorizontalSpacing(12);
    grid->setColumnStretch(1, 1);
    if (req.kind == PromptRequest::Kind::Conflict) {
        addPathRow(grid, 0, tr("Source"), req.srcPath, describe(req.srcSize, req.srcTime));
        addPathRow(grid, 2, tr("Target"), req.dstPath, describe(req.dstSize, req.dstTime));
        if (req.srcTime > req.dstTime) {
            auto* hint = new QLabel(tr("The source is newer."), this);
            setThemeStyle(hint, QStringLiteral("color: @success;"));
            grid->addWidget(hint, 4, 1);
        } else if (req.srcTime < req.dstTime) {
            auto* hint = new QLabel(tr("The target is newer."), this);
            setThemeStyle(hint, QStringLiteral("color: @mark;"));
            grid->addWidget(hint, 4, 1);
        }
    } else {
        addPathRow(grid, 0, tr("Path"), req.srcPath, QString());
        if (!req.detail.isEmpty()) {
            auto* d = new QLabel(req.detail, this);
            setThemeStyle(d, QStringLiteral("color: @error;"));
            grid->addWidget(d, 1, 1);
        }
    }
    layout->addLayout(grid);

    auto* row1 = new QHBoxLayout;
    auto* row2 = new QHBoxLayout;
    auto add = [this](QHBoxLayout* row, const QString& text, Answer a, bool primary = false) {
        auto* b = new QPushButton(text, this);
        b->setAutoDefault(false);
        if (primary) {
            b->setDefault(true);
            b->setFocus();
        }
        connect(b, &QPushButton::clicked, this, [this, a] { done(int(a)); });
        row->addWidget(b);
    };
    switch (req.kind) {
    case PromptRequest::Kind::Conflict:
        add(row1, tr("&Overwrite"), Answer::Overwrite, true);
        add(row1, tr("Overwrite &all"), Answer::OverwriteAll);
        add(row1, tr("Overwrite all &older"), Answer::OverwriteOlderAll);
        add(row2, tr("&Skip"), Answer::Skip);
        add(row2, tr("S&kip all"), Answer::SkipAll);
        add(row2, tr("&Rename"), Answer::Rename);
        add(row2, tr("Rename a&ll"), Answer::RenameAll);
        break;
    case PromptRequest::Kind::Error:
        add(row1, tr("&Retry"), Answer::Retry, true);
        add(row1, tr("&Skip"), Answer::Skip);
        add(row1, tr("S&kip all"), Answer::SkipAll);
        break;
    case PromptRequest::Kind::TrashFailed:
        add(row1, tr("&Delete permanently"), Answer::DeletePermanently, true);
        add(row1, tr("Delete &all permanently"), Answer::DeletePermanentlyAll);
        add(row1, tr("&Skip"), Answer::Skip);
        break;
    }
    row1->addStretch();
    row2->addStretch();
    add(row2, tr("&Cancel operation"), Answer::Cancel);  // Esc also maps to Cancel (0)
    layout->addLayout(row1);
    layout->addLayout(row2);
}

}  // namespace tandem
