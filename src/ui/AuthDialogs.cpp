#include "ui/AuthDialogs.h"

#include "core/Backend.h"
#include "core/Keyring.h"
#include "core/Util.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QVBoxLayout>

#include <atomic>
#include <chrono>
#include <future>

namespace tandem {

namespace {

std::atomic<bool> g_quitting{false};

QWidget* dialogParent()
{
    if (QWidget* w = QApplication::activeWindow())
        return w;
    // Between two dialogs nothing is active: stay on the main window anyway.
    for (QWidget* w : QApplication::topLevelWidgets())
        if (w->inherits("QMainWindow") && w->isVisible())
            return w;
    return nullptr;
}

QDialog* makeDialog(const AuthQuestion& q, QLineEdit** editOut, QLineEdit** userOut, QCheckBox** rememberOut)
{
    auto* dlg = new QDialog(dialogParent());
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setObjectName(QStringLiteral("authDialog"));
    const bool hostKey = q.kind == AuthQuestion::Kind::HostKey;
    dlg->setWindowTitle(hostKey ? QObject::tr("Unknown server") : QObject::tr("Log in"));
    dlg->setMinimumWidth(420);
    auto* layout = new QVBoxLayout(dlg);
    auto* where = new QLabel(q.location, dlg);
    setThemeStyle(where, (QStringLiteral("color: @dimText;")));
    auto* text = new QLabel(q.message, dlg);
    text->setWordWrap(true);
    layout->addWidget(where);
    layout->addWidget(text);
    if (!q.detail.isEmpty()) {
        auto* detail = new QLabel(q.detail, dlg);
        detail->setFont(monoFont());
        detail->setTextInteractionFlags(Qt::TextSelectableByMouse);
        detail->setWordWrap(true);
        layout->addWidget(detail);
    }
    QLineEdit* edit = nullptr;
    QLineEdit* user = nullptr;
    if (q.kind == AuthQuestion::Kind::Login) {
        auto* form = new QFormLayout;
        user = new QLineEdit(q.user, dlg);
        edit = new QLineEdit(dlg);
        edit->setEchoMode(QLineEdit::Password);
        form->addRow(QObject::tr("User"), user);
        form->addRow(QObject::tr("Password"), edit);
        layout->addLayout(form);
        if (q.user.isEmpty())
            user->setFocus();
        else
            edit->setFocus();
    } else if (!hostKey) {
        edit = new QLineEdit(dlg);
        if (q.secret)
            edit->setEchoMode(QLineEdit::Password);
        layout->addWidget(edit);
        edit->setFocus();
    } else {
        auto* hint = new QLabel(QObject::tr("Compare it with what the server's admin gives you. Tandem remembers it "
                                            "in ~/.ssh/known_hosts, like ssh does."),
                                dlg);
        hint->setWordWrap(true);
        setThemeStyle(hint, (QStringLiteral("color: @dimText;")));
        layout->addWidget(hint);
    }
    QCheckBox* remember = nullptr;
    if (q.offerRemember && keyringAvailable()) {
        remember = new QCheckBox(QObject::tr("Remember in the keyring"), dlg);
        remember->setToolTip(QObject::tr("Stored by your desktop (GNOME Keyring, KWallet); file managers like Nautilus "
                                         "find it too"));
        layout->addWidget(remember);
    }
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dlg);
    buttons->button(QDialogButtonBox::Ok)->setText(hostKey ? QObject::tr("Trust and connect") : QObject::tr("Log in"));
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, dlg, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, dlg, &QDialog::reject);
    *editOut = edit;
    *userOut = user;
    *rememberOut = remember;
    return dlg;
}

}  // namespace

void installAuthDialogs()
{
    QObject::connect(qApp, &QCoreApplication::aboutToQuit, [] { g_quitting = true; });
    setAuthHandler([](const AuthQuestion& q) -> std::optional<QString> {
        auto promise = std::make_shared<std::promise<std::optional<QString>>>();
        auto answered = std::make_shared<std::atomic<bool>>(false);
        auto reply = [promise, answered](std::optional<QString> a) {
            if (!answered->exchange(true))
                promise->set_value(std::move(a));
        };
        std::future<std::optional<QString>> result = promise->get_future();
        postToUi([q, reply] {
            QLineEdit* edit = nullptr;
            QLineEdit* user = nullptr;
            QCheckBox* remember = nullptr;
            QDialog* dlg = makeDialog(q, &edit, &user, &remember);
            QPointer<QLineEdit> e(edit);
            QPointer<QLineEdit> u(user);
            QPointer<QCheckBox> keep(remember);
            QObject::connect(dlg, &QDialog::finished, [reply, e, u, keep](int r) {
                if (r != QDialog::Accepted) {
                    reply(std::nullopt);
                    return;
                }
                QString answer = u ? u->text().trimmed() + QLatin1Char('\n') + (e ? e->text() : QString())
                                   : (e ? e->text() : QStringLiteral("yes"));
                if (keep && keep->isChecked())
                    answer += QLatin1String(kRememberMark);
                reply(answer);
            });
            QObject::connect(dlg, &QObject::destroyed, [reply] { reply(std::nullopt); });
            dlg->open();
        });
        while (result.wait_for(std::chrono::milliseconds(200)) != std::future_status::ready)
            if (g_quitting)
                return std::nullopt;
        return result.get();
    });
}

}  // namespace tandem
