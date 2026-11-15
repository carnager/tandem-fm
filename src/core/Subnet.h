#pragma once

#include <QList>
#include <QString>

#include <array>
#include <optional>

namespace tandem {

// A network like "192.168.1.0/24" or "fd00::/64".
struct Subnet {
    std::array<unsigned char, 16> bytes{};
    bool v6 = false;
    int prefix = 0;

    // Addresses only, never a name to look up. A bare address is a /32 (/128).
    static std::optional<Subnet> parse(const QString& text);
    bool contains(const Subnet& address) const;
    QString toString() const;  // the network: "192.168.1.0/24"
};

// "192.168.1.0/24, fd00::/64": empty on a mistake, with what's wrong.
QList<Subnet> parseNetworks(const QString& text, QString* error = nullptr);

// The addresses of this computer's network connections, each with its
// network size. Not loopback, and not VPNs (point-to-point links): a VPN
// address doesn't mean the computer is in that network.
QList<Subnet> localAddresses();
// Tests pick the addresses; nullopt: the real ones again.
void setLocalAddressesForTests(const std::optional<QList<Subnet>>& addresses);

// Whether this computer is in one of the networks now.
bool inNetworks(const QString& networks);

// The home-style network (private IPv4) the computer is in now, as
// "192.168.1.0/24", to offer as "direct in network"; empty if none.
QString currentNetwork();

}  // namespace tandem
