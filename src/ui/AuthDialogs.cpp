#include "ui/AuthDialogs.h"

#include "core/Backend.h"
#include "core/Util.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
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

QDialog* makeDialog(const AuthQuestion& q, QLineEdit** editOut)
{
    auto* dlg = new QDialog(QApplication::activeWindow());
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setObjectName(QStringLiteral("authDialog"));
    const bool hostKey = q.kind == AuthQuestion::Kind::HostKey;
    dlg->setWindowTitle(hostKey ? QObject::tr("Unknown server") : QObject::tr("Log in"));
    dlg->setMinimumWidth(420);
    auto* layout = new QVBoxLayout(dlg);
    auto* where = new QLabel(q.location, dlg);
    where->setStyleSheet(theme().css(QStringLiteral("color: @dimText;")));
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
    if (!hostKey) {
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
        hint->setStyleSheet(theme().css(QStringLiteral("color: @dimText;")));
        layout->addWidget(hint);
    }
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dlg);
    buttons->button(QDialogButtonBox::Ok)->setText(hostKey ? QObject::tr("Trust and connect") : QObject::tr("Log in"));
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, dlg, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, dlg, &QDialog::reject);
    *editOut = edit;
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
            QDialog* dlg = makeDialog(q, &edit);
            QPointer<QLineEdit> e(edit);
            QObject::connect(dlg, &QDialog::finished, [reply, e](int r) {
                if (r != QDialog::Accepted)
                    reply(std::nullopt);
                else
                    reply(e ? e->text() : QStringLiteral("yes"));
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
