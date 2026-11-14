#include "ui/MacModalKeys.h"
#include "ui/Keymap.h"

#include <QKeyEvent>

namespace tandem {

QString macPhysicalModalToken(const QKeyEvent* event)
{
    if (!event->key() || event->key() == Qt::Key_unknown)
        return {};  // composed text is not a physical command key
    if (!event->spontaneous() && !event->nativeVirtualKey() && !event->nativeScanCode() &&
        !event->nativeModifiers())
        return {};  // a synthesized event without native information

    // Apple's ANSI virtual key codes (NSEvent.keyCode). A is genuinely zero.
    struct Key { quint32 code; char plain, shifted; };
    static constexpr Key keys[] = {
        {0,'a','A'}, {1,'s','S'}, {2,'d','D'}, {3,'f','F'}, {4,'h','H'}, {5,'g','G'},
        {6,'z','Z'}, {7,'x','X'}, {8,'c','C'}, {9,'v','V'}, {11,'b','B'},
        {12,'q','Q'}, {13,'w','W'}, {14,'e','E'}, {15,'r','R'}, {16,'y','Y'}, {17,'t','T'},
        {18,'1','!'}, {19,'2','@'}, {20,'3','#'}, {21,'4','$'}, {22,'6','^'}, {23,'5','%'},
        {24,'=','+'}, {25,'9','('}, {26,'7','&'}, {27,'-','_'}, {28,'8','*'}, {29,'0',')'},
        {30,']','}'}, {31,'o','O'}, {32,'u','U'}, {33,'[','{'}, {34,'i','I'}, {35,'p','P'},
        {37,'l','L'}, {38,'j','J'}, {39,'\'','"'}, {40,'k','K'}, {41,';',':'}, {42,'\\','|'},
        {43,',','<'}, {44,'/','?'}, {45,'n','N'}, {46,'m','M'}, {47,'.','>'}, {50,'`','~'},
    };
    for (const Key& key : keys) {
        if (key.code != event->nativeVirtualKey())
            continue;
        bool shifted = event->modifiers().testFlag(Qt::ShiftModifier);
        if (key.plain >= 'a' && key.plain <= 'z')
            shifted ^= bool(event->nativeModifiers() & (1u << 16));  // Cocoa Caps Lock
        const QChar text = QLatin1Char(shifted ? key.shifted : key.plain);
        QKeyEvent translated(event->type(), text.toUpper().unicode(), event->modifiers(), QString(text));
        return Keymap::tokenFor(&translated);
    }
    return {};
}

}  // namespace tandem
