#include "ui/Activation.h"

#include "xdg-activation-v1-client-protocol.h"

#include <QElapsedTimer>
#include <QGuiApplication>
#include <QThread>
#include <QtGui/qguiapplication_platform.h>

#include <cstring>
#include <wayland-client.h>

namespace tandem {

namespace {

// Our own event queue on Qt's connection: waiting for the token dispatches
// only our events, never Qt's.
struct Activation {
    wl_event_queue* queue = nullptr;
    xdg_activation_v1* manager = nullptr;
};

void addGlobal(void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t)
{
    if (std::strcmp(interface, xdg_activation_v1_interface.name) == 0)
        static_cast<Activation*>(data)->manager =
            static_cast<xdg_activation_v1*>(wl_registry_bind(registry, name, &xdg_activation_v1_interface, 1));
}

void removeGlobal(void*, wl_registry*, uint32_t) {}

const wl_registry_listener kRegistry = {addGlobal, removeGlobal};

void tokenDone(void* data, xdg_activation_token_v1*, const char* token)
{
    *static_cast<QString*>(data) = QString::fromUtf8(token);
}

const xdg_activation_token_v1_listener kToken = {tokenDone};

// Looks for the protocol once, on the first launch.
Activation* activation(wl_display* display)
{
    static Activation a;
    static bool looked = false;
    if (!looked) {
        looked = true;
        a.queue = wl_display_create_queue(display);
        auto* wrapped = static_cast<wl_display*>(wl_proxy_create_wrapper(display));
        wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(wrapped), a.queue);
        wl_registry* registry = wl_display_get_registry(wrapped);
        wl_proxy_wrapper_destroy(wrapped);
        wl_registry_add_listener(registry, &kRegistry, &a);
        wl_display_roundtrip_queue(display, a.queue);
        wl_registry_destroy(registry);
    }
    return a.manager ? &a : nullptr;
}

}  // namespace

QString requestActivationToken()
{
    if (!qGuiApp || QThread::currentThread() != qGuiApp->thread())
        return {};
    auto* app = qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>();
    if (!app || !app->display())
        return {};
    Activation* a = activation(app->display());
    if (!a)
        return {};
    QString token;
    xdg_activation_token_v1* request = xdg_activation_v1_get_activation_token(a->manager);
    xdg_activation_token_v1_add_listener(request, &kToken, &token);
    // The key press or click that asked for the launch: compositors only
    // hand focus on for what the user just did in the focused app.
    if (const uint serial = app->lastInputSerial()) {
        wl_seat* seat = app->lastInputSeat() ? app->lastInputSeat() : app->seat();
        if (seat)
            xdg_activation_token_v1_set_serial(request, serial, seat);
    }
    xdg_activation_token_v1_commit(request);
    QElapsedTimer waited;
    waited.start();
    while (token.isEmpty() && waited.elapsed() < 500)
        if (wl_display_roundtrip_queue(app->display(), a->queue) < 0)
            break;
    xdg_activation_token_v1_destroy(request);
    return token;
}

}  // namespace tandem
