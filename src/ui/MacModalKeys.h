#pragma once

#include <QString>
class QKeyEvent;

namespace tandem {
// US key positions from Cocoa's native key code; empty for unmapped keys.
QString macPhysicalModalToken(const QKeyEvent* event);
}
