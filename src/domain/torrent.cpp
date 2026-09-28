#include "domain/torrent.h"

#include "common/infohash.h"

#include <QUrl>
#include <QUrlQuery>

namespace rats::domain {

bool Torrent::isValid() const
{
    return infohash::isValid(hash);
}

QString Torrent::magnetLink() const
{
    // A tracker can publish announce URLs (and other metadata) in its own
    // magnet. Keep them once the exact release and info-hash were verified.
    if (info.value(QStringLiteral("sourceVerified")).toBool()) {
        QString source = info.value(QStringLiteral("sourceMagnet")).toString();
        source.replace(QStringLiteral("&amp;"), QStringLiteral("&"));
        const QUrl url(source);
        if (url.isValid() && url.scheme() == QStringLiteral("magnet")) {
            const QUrlQuery query(url);
            bool hasExactHash = false;
            bool hasOtherHash = false;
            for (const auto& item : query.queryItems(QUrl::FullyDecoded)) {
                if (item.first.compare(QStringLiteral("xt"), Qt::CaseInsensitive) != 0)
                    continue;
                const QString prefix = QStringLiteral("urn:btih:");
                if (item.second.startsWith(prefix, Qt::CaseInsensitive)
                    && infohash::normalize(item.second.mid(prefix.size())) == infohash::normalize(hash))
                    hasExactHash = true;
                else
                    hasOtherHash = true;
            }
            if (hasExactHash && !hasOtherHash && isValid())
                return source;
        }
    }
    return QStringLiteral("magnet:?xt=urn:btih:%1&dn=%2").arg(hash, QString::fromLatin1(QUrl::toPercentEncoding(name)));
}

} // namespace rats::domain
