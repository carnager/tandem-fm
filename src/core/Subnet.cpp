#include "core/Subnet.h"

#include <QMutex>
#include <QObject>
#include <QRegularExpression>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <cstring>

namespace tandem {

namespace {

QMutex g_testMutex;
std::optional<QList<Subnet>> g_testAddresses;

int prefixOf(const unsigned char* mask, int len)
{
    int bits = 0;
    for (int i = 0; i < len; ++i)
        for (int b = 7; b >= 0 && (mask[i] >> b & 1); --b)
            ++bits;
    return bits;
}

}  // namespace

std::optional<Subnet> Subnet::parse(const QString& textIn)
{
    const QString text = textIn.trimmed();
    if (text.isEmpty())
        return std::nullopt;
    const qsizetype slash = text.indexOf(QLatin1Char('/'));
    const QByteArray addr = (slash < 0 ? text : text.left(slash)).toLatin1();
    Subnet s;
    if (inet_pton(AF_INET, addr.constData(), s.bytes.data()) == 1) {
        s.v6 = false;
    } else if (inet_pton(AF_INET6, addr.constData(), s.bytes.data()) == 1) {
        s.v6 = true;
    } else {
        return std::nullopt;
    }
    const int max = s.v6 ? 128 : 32;
    s.prefix = max;
    if (slash >= 0) {
        bool ok = false;
        s.prefix = text.mid(slash + 1).toInt(&ok);
        if (!ok || s.prefix < 0 || s.prefix > max)
            return std::nullopt;
    }
    return s;
}

bool Subnet::contains(const Subnet& a) const
{
    if (a.v6 != v6)
        return false;
    int bits = prefix;
    for (int i = 0; bits > 0; ++i, bits -= 8) {
        const unsigned char mask = bits >= 8 ? 0xff : static_cast<unsigned char>(0xff << (8 - bits));
        if ((bytes[i] & mask) != (a.bytes[i] & mask))
            return false;
    }
    return true;
}

QString Subnet::toString() const
{
    std::array<unsigned char, 16> net{};
    int bits = prefix;
    for (int i = 0; bits > 0; ++i, bits -= 8)
        net[i] = bytes[i] & (bits >= 8 ? 0xff : static_cast<unsigned char>(0xff << (8 - bits)));
    char buf[INET6_ADDRSTRLEN] = {};
    inet_ntop(v6 ? AF_INET6 : AF_INET, net.data(), buf, sizeof buf);
    return QStringLiteral("%1/%2").arg(QString::fromLatin1(buf)).arg(prefix);
}

QList<Subnet> parseNetworks(const QString& text, QString* error)
{
    QList<Subnet> out;
    static const QRegularExpression sep(QStringLiteral("[,\\s]+"));
    for (const QString& part : text.split(sep, Qt::SkipEmptyParts)) {
        const auto s = Subnet::parse(part);
        if (!s) {
            if (error)
                *error = QObject::tr("\"%1\" isn't a network like 192.168.1.0/24").arg(part);
            return {};
        }
        out << *s;
    }
    return out;
}

QList<Subnet> localAddresses()
{
    {
        QMutexLocker lock(&g_testMutex);
        if (g_testAddresses)
            return *g_testAddresses;
    }
    QList<Subnet> out;
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0)
        return out;
    for (ifaddrs* i = list; i; i = i->ifa_next) {
        if (!i->ifa_addr || !i->ifa_netmask)
            continue;
        if (!(i->ifa_flags & IFF_UP) || !(i->ifa_flags & IFF_RUNNING) || (i->ifa_flags & (IFF_LOOPBACK | IFF_POINTOPOINT)))
            continue;
        Subnet s;
        if (i->ifa_addr->sa_family == AF_INET) {
            memcpy(s.bytes.data(), &reinterpret_cast<sockaddr_in*>(i->ifa_addr)->sin_addr, 4);
            s.prefix = prefixOf(reinterpret_cast<unsigned char*>(&reinterpret_cast<sockaddr_in*>(i->ifa_netmask)->sin_addr), 4);
        } else if (i->ifa_addr->sa_family == AF_INET6) {
            s.v6 = true;
            memcpy(s.bytes.data(), &reinterpret_cast<sockaddr_in6*>(i->ifa_addr)->sin6_addr, 16);
            s.prefix = prefixOf(reinterpret_cast<unsigned char*>(&reinterpret_cast<sockaddr_in6*>(i->ifa_netmask)->sin6_addr), 16);
        } else {
            continue;
        }
        out << s;
    }
    freeifaddrs(list);
    return out;
}

void setLocalAddressesForTests(const std::optional<QList<Subnet>>& addresses)
{
    QMutexLocker lock(&g_testMutex);
    g_testAddresses = addresses;
}

bool inNetworks(const QString& networks)
{
    const QList<Subnet> nets = parseNetworks(networks);
    if (nets.isEmpty())
        return false;
    for (const Subnet& a : localAddresses())
        for (const Subnet& n : nets)
            if (n.contains(a))
                return true;
    return false;
}

QString currentNetwork()
{
    // Home networks first; 172.16/12 last, it's mostly containers.
    const QList<Subnet> ranges = parseNetworks(QStringLiteral("192.168.0.0/16 10.0.0.0/8 172.16.0.0/12"));
    for (const Subnet& range : ranges)
        for (const Subnet& a : localAddresses())
            if (range.contains(a) && a.prefix >= 8 && a.prefix <= 30)
                return a.toString();
    return {};
}

}  // namespace tandem
