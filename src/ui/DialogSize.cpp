#include "ui/DialogSize.h"

#include <QEvent>
#include <QGuiApplication>
#include <QScreen>
#include <QSettings>
#include <QWidget>

namespace tandem {

namespace {

class SizeKeeper : public QObject {
public:
    SizeKeeper(QWidget* window, QString key) : QObject(window), m_key(std::move(key)) {}

protected:
    bool eventFilter(QObject* obj, QEvent* event) override
    {
        auto* w = static_cast<QWidget*>(obj);
        if (event->type() == QEvent::Hide && !w->isMaximized() && !w->isFullScreen())
            QSettings().setValue(m_key, QVariantList{w->width(), w->height()});
        return false;
    }

private:
    QString m_key;
};

}  // namespace

void rememberSize(QWidget* window, const QString& key)
{
    const QString settingsKey = QStringLiteral("dialogs/") + key;
    const QVariantList saved = QSettings().value(settingsKey).toList();
    QSize size = saved.size() == 2 ? QSize(saved[0].toInt(), saved[1].toInt()) : QSize();
    if (size.isValid() && !size.isEmpty()) {
        // Never larger than the screen it opens on (a monitor may be gone).
        if (QScreen* screen = window->screen() ? window->screen() : QGuiApplication::primaryScreen())
            size = size.boundedTo(screen->availableSize());
        window->resize(size);
    }
    window->installEventFilter(new SizeKeeper(window, settingsKey));
}

}  // namespace tandem
