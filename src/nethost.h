#pragma once

// Server addresses for CloudSync and updates.

#include <QHostAddress>
#include <QString>
#include <QUrl>

// A host on this computer or the local network: localhost, a private or link-local IP address, a
// name without dots, or a .local / .lan / .home name. Those may use plain http; anything on the
// internet should use https.
inline bool isLocalNetworkHost(const QString& hostOrUrl) {
  QString host = hostOrUrl.trimmed();
  if (host.contains(QStringLiteral("://"))) host = QUrl(host).host();
  else host = QUrl(QStringLiteral("http://") + host).host();
  if (host.isEmpty()) return false;
  const QString lower = host.toLower();
  if (lower == QLatin1String("localhost") || !lower.contains(QLatin1Char('.')) || lower.endsWith(QLatin1String(".local")) ||
      lower.endsWith(QLatin1String(".lan")) || lower.endsWith(QLatin1String(".home")))
    return true;
  const QHostAddress address(host);
  if (address.isNull()) return false;
  if (address.isLoopback() || address.isLinkLocal()) return true;
  return address.isInSubnet(QHostAddress(QStringLiteral("10.0.0.0")), 8) ||
         address.isInSubnet(QHostAddress(QStringLiteral("172.16.0.0")), 12) ||
         address.isInSubnet(QHostAddress(QStringLiteral("192.168.0.0")), 16) ||
         address.isInSubnet(QHostAddress(QStringLiteral("100.64.0.0")), 10) ||  // Tailscale and similar
         address.isInSubnet(QHostAddress(QStringLiteral("fc00::")), 7);
}

// Without a scheme: https, except for a local network host (often plain http behind no proxy).
inline QString withScheme(const QString& host) {
  if (host.startsWith(QStringLiteral("http://"), Qt::CaseInsensitive) || host.startsWith(QStringLiteral("https://"), Qt::CaseInsensitive))
    return host;
  return (isLocalNetworkHost(host) ? QStringLiteral("http://") : QStringLiteral("https://")) + host;
}
