#include "ui/Dialogs.h"

#include "core/Archive.h"
#include "core/Util.h"
#include "ui/Theme.h"

#include <QDateTime>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
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

std::optional<PackChoice> askPack(QWidget* parent, const QString& what, const QString& base)
{
    struct Format {
        const char* ext;
        const char* label;
    };
    static const Format formats[] = {
        {".zip", QT_TRANSLATE_NOOP("QObject", "ZIP")},          {".7z", QT_TRANSLATE_NOOP("QObject", "7-Zip")},
        {".tar.gz", QT_TRANSLATE_NOOP("QObject", "tar.gz")},    {".tar.xz", QT_TRANSLATE_NOOP("QObject", "tar.xz")},
        {".tar.zst", QT_TRANSLATE_NOOP("QObject", "tar.zst")}, {".tar", QT_TRANSLATE_NOOP("QObject", "tar")},
    };
    auto extOf = [](const QString& path) -> int {  // index into formats, -1: none of them
        int best = -1;
        for (int i = 0; i < int(std::size(formats)); ++i)
            if (path.endsWith(QLatin1String(formats[i].ext), Qt::CaseInsensitive) &&
                (best < 0 || qstrlen(formats[i].ext) > qstrlen(formats[best].ext)))
                best = i;
        return best;
    };

    QDialog dlg(parent);
    dlg.setWindowTitle(QObject::tr("Pack"));
    dlg.setMinimumWidth(600);
    auto* layout = new QVBoxLayout(&dlg);
    layout->setContentsMargins(18, 16, 18, 14);
    layout->setSpacing(10);
    auto* intro = new QLabel(QObject::tr("Pack %1 into:").arg(what), &dlg);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto* form = new QFormLayout;
    form->setHorizontalSpacing(12);
    auto* target = new QLineEdit(&dlg);
    target->setObjectName(QStringLiteral("target"));
    auto* format = new QComboBox(&dlg);
    format->setObjectName(QStringLiteral("format"));
    for (const Format& f : formats)
        format->addItem(QStringLiteral("%1   (%2)").arg(QObject::tr(f.label), QLatin1String(f.ext)));
    auto* password = new QLineEdit(&dlg);
    password->setObjectName(QStringLiteral("password"));
    password->setEchoMode(QLineEdit::Password);
    password->setPlaceholderText(QObject::tr("none"));
    auto* repeat = new QLineEdit(&dlg);
    repeat->setObjectName(QStringLiteral("repeat"));
    repeat->setEchoMode(QLineEdit::Password);
    auto* note = new QLabel(&dlg);
    note->setWordWrap(true);
    form->addRow(QObject::tr("&Archive"), target);
    form->addRow(QObject::tr("&Format"), format);
    form->addRow(QObject::tr("&Password"), password);
    form->addRow(QObject::tr("&Repeat"), repeat);
    form->addRow(QString(), note);
    layout->addLayout(form);

    auto* hint = new QLabel(QObject::tr("Enter: queue behind other operations on the same drive   ·   "
                                        "Shift+Enter: start right away"),
                            &dlg);
    setThemeStyle(hint, QStringLiteral("color: @dimText;"));
    layout->addWidget(hint);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    buttons->button(QDialogButtonBox::Ok)->setText(QObject::tr("Pack"));
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    layout->addWidget(buttons);

    // The format and the name's extension follow each other.
    bool syncing = false;
    auto update = [&] {
        const QString ext = QLatin1String(formats[format->currentIndex()].ext);
        const bool zip = ext == QLatin1String(".zip"), sevenZip = ext == QLatin1String(".7z");
        const bool can = canEncryptArchive(QStringLiteral("x") + ext);
        password->setEnabled(can);
        repeat->setEnabled(can && !password->text().isEmpty());
        QString text;
        bool bad = false;
        if (!can && sevenZip) {
            text = QObject::tr("A password for 7z needs 7-Zip (7z or 7zz) installed.");
        } else if (!can) {
            text = QObject::tr("tar archives can't have a password: pick ZIP or 7-Zip for one.");
        } else if (!password->text().isEmpty() && password->text() != repeat->text()) {
            text = repeat->text().isEmpty() ? QObject::tr("Type the password again.")
                                            : QObject::tr("The passwords don't match.");
            bad = !repeat->text().isEmpty();
        } else if (zip) {
            text = QObject::tr("ZIP encrypts the files with AES-256; their names stay readable.");
        } else {
            text = QObject::tr("7-Zip encrypts the files and their names with AES-256.");
        }
        note->setText(text);
        setThemeStyle(note, bad ? QStringLiteral("color: @error;") : QStringLiteral("color: @dimText;"));
        const bool matches = password->text().isEmpty() || !can || password->text() == repeat->text();
        buttons->button(QDialogButtonBox::Ok)->setEnabled(!target->text().trimmed().isEmpty() && matches);
    };
    QObject::connect(format, &QComboBox::currentIndexChanged, &dlg, [&](int i) {
        if (!syncing) {
            syncing = true;
            QString t = target->text();
            if (const int old = extOf(t); old >= 0)
                t.chop(qstrlen(formats[old].ext));
            target->setText(t + QLatin1String(formats[i].ext));
            syncing = false;
        }
        QSettings().setValue(QStringLiteral("pack/format"), QLatin1String(formats[i].ext));
        update();
    });
    QObject::connect(target, &QLineEdit::textEdited, &dlg, [&](const QString& t) {
        if (const int i = extOf(t); i >= 0 && i != format->currentIndex()) {
            syncing = true;
            format->setCurrentIndex(i);
            syncing = false;
        }
        update();
    });
    QObject::connect(password, &QLineEdit::textChanged, &dlg, update);
    QObject::connect(repeat, &QLineEdit::textChanged, &dlg, update);

    const int last = extOf(QSettings().value(QStringLiteral("pack/format"), QStringLiteral(".zip")).toString());
    const int start = last < 0 ? 0 : last;
    target->setText(base + QLatin1String(formats[start].ext));
    {
        const QSignalBlocker block(format);
        format->setCurrentIndex(start);
    }
    // The name selected, so typing replaces it; the extension stays.
    const QString name = baseName(base);
    target->setSelection(int(target->text().size() - qstrlen(formats[start].ext) - name.size()), int(name.size()));
    update();

    bool now = false;
    for (auto key : {Qt::SHIFT | Qt::Key_Return, Qt::SHIFT | Qt::Key_Enter}) {
        auto* sc = new QShortcut(QKeySequence(key), &dlg);
        QObject::connect(sc, &QShortcut::activated, &dlg, [&] {
            if (buttons->button(QDialogButtonBox::Ok)->isEnabled()) {
                now = true;
                dlg.accept();
            }
        });
    }
    if (dlg.exec() != QDialog::Accepted)
        return std::nullopt;
    PackChoice c;
    c.target = target->text().trimmed();
    c.password = password->isEnabled() ? password->text() : QString();
    c.startNow = now;
    return c;
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
    case PromptRequest::Kind::Conflict: {
        // Enter must not throw away a newer file.
        const bool targetNewer = req.dstTime > req.srcTime;
        add(row1, tr("&Overwrite"), Answer::Overwrite, !targetNewer);
        add(row1, tr("Overwrite &all"), Answer::OverwriteAll);
        add(row1, tr("Overwrite all &older"), Answer::OverwriteOlderAll);
        add(row2, tr("&Skip"), Answer::Skip, targetNewer);
        add(row2, tr("S&kip all"), Answer::SkipAll);
        add(row2, tr("Keep &both"), Answer::Rename);
        add(row2, tr("Keep both for a&ll"), Answer::RenameAll);
        break;
    }
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
