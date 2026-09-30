#include "library/autodj/smart/bridgefinder.h"

#include <cmath>

#include <QRegularExpression>
#include <algorithm>
#include <random>

namespace {

// Lower case, "&" -> "and", everything that is not a letter or digit
// becomes a space, spaces collapsed.
QString tidy(QString text) {
    text = text.toLower();
    text.replace(QChar('&'), QStringLiteral(" and "));
    QString out;
    out.reserve(text.size());
    for (const QChar c : std::as_const(text)) {
        out.append(c.isLetterOrNumber() ? c : QChar(' '));
    }
    return out.simplified();
}

QString withoutLeadingThe(const QString& text) {
    return text.startsWith(QStringLiteral("the ")) ? text.mid(4) : text;
}

// Genre families. A tag can belong to several ("Rock/Pop", "Electro").
enum GenreFamily : unsigned {
    kDark = 1u << 0,       // goth, darkwave, post-punk
    kIndustrial = 1u << 1, // industrial, EBM
    kNewWave = 1u << 2,    // new wave, synthpop, 80s, italo
    kDance = 1u << 3,      // dance, electronic, house, techno, disco
    kRock = 1u << 4,       // rock, alternative, indie
    kPop = 1u << 5,
    kHipHop = 1u << 6, // hip hop, rap, r&b
};

unsigned genreFamilies(const QString& tidyGenre) {
    struct Word {
        const char* text;
        unsigned families;
    };
    static const Word kWords[] = {
            {"goth", kDark},
            {"gothic", kDark},
            {"darkwave", kDark},
            {"dark", kDark},
            {"deathrock", kDark},
            {"post punk", kDark},
            {"postpunk", kDark},
            {"coldwave", kDark},
            {"industrial", kIndustrial},
            {"ebm", kIndustrial},
            {"electronic body music", kIndustrial},
            {"electro industrial", kIndustrial},
            {"aggrotech", kIndustrial},
            {"new wave", kNewWave},
            {"synthpop", kNewWave},
            {"synth pop", kNewWave},
            {"synthwave", kNewWave},
            {"new romantic", kNewWave},
            {"80s", kNewWave},
            {"80 s", kNewWave},
            {"retro", kNewWave},
            {"italo", kNewWave},
            {"electro", kNewWave | kDance},
            {"dance", kDance},
            {"electronic", kDance},
            {"house", kDance},
            {"techno", kDance},
            {"disco", kDance},
            {"new beat", kDance},
            {"trance", kDance},
            {"rock", kRock},
            {"alternative", kRock},
            {"indie", kRock},
            {"punk", kRock},
            {"pop", kPop},
            {"hip hop", kHipHop},
            {"hiphop", kHipHop},
            {"rap", kHipHop},
            {"r and b", kHipHop},
            {"rnb", kHipHop},
    };
    const QString padded = QChar(' ') + tidyGenre + QChar(' ');
    unsigned families = 0;
    for (const Word& w : kWords) {
        if (padded.contains(QChar(' ') + QLatin1String(w.text) + QChar(' '))) {
            families |= w.families;
        }
    }
    return families;
}

// Families that mix well into each other.
unsigned relatedFamilies(unsigned families) {
    unsigned related = 0;
    if (families & kDark) {
        related |= kIndustrial | kNewWave | kRock;
    }
    if (families & kIndustrial) {
        related |= kDark | kNewWave | kDance;
    }
    if (families & kNewWave) {
        related |= kDark | kIndustrial | kDance | kPop;
    }
    if (families & kDance) {
        related |= kNewWave | kIndustrial | kPop;
    }
    if (families & kRock) {
        related |= kDark | kPop;
    }
    if (families & kPop) {
        related |= kNewWave | kDance | kRock;
    }
    return related;
}

bool isUnknownGenre(const QString& tidyGenre) {
    return tidyGenre.isEmpty() || tidyGenre == QStringLiteral("other") ||
            tidyGenre == QStringLiteral("unknown") || tidyGenre == QStringLiteral("remix") ||
            tidyGenre.startsWith(QStringLiteral("general ")) ||
            tidyGenre.contains(QStringLiteral("megamix"));
}

} // namespace

// static
double BridgeFinder::genreCost(const QString& genreA, const QString& genreB) {
    const QString a = tidy(genreA);
    const QString b = tidy(genreB);
    if (isUnknownGenre(a) || isUnknownGenre(b)) {
        // "General Post-Punk" still says something: use its families.
        const unsigned fa = genreFamilies(a);
        const unsigned fb = genreFamilies(b);
        if (fa && fb) {
            return (fa & fb) ? 0.0 : ((relatedFamilies(fa) & fb) ? kRelatedGenreCost : kOtherGenreCost);
        }
        return kUnknownGenreCost;
    }
    if (a == b) {
        return 0.0;
    }
    const unsigned fa = genreFamilies(a);
    const unsigned fb = genreFamilies(b);
    if (!fa || !fb) {
        return kUnknownGenreCost; // a genre we have no family for
    }
    if (fa & fb) {
        return 0.0;
    }
    return (relatedFamilies(fa) & fb) ? kRelatedGenreCost : kOtherGenreCost;
}

// static
QString BridgeFinder::normalizeArtist(const QString& artist) {
    return withoutLeadingThe(tidy(artist));
}

// static
QString BridgeFinder::normalizeTitle(const QString& title, const QString& normalizedArtist) {
    QString t = title;
    // Bracketed tags: (12" Version), [Remastered], {Live}.
    static const QRegularExpression kBrackets(QStringLiteral("[\\(\\[\\{][^\\)\\]\\}]*[\\)\\]\\}]"));
    t.remove(kBrackets);
    // A version ending after " - ": "Blue Monday - 1988 Remix".
    static const QRegularExpression kVersionEnding(QStringLiteral(
            "\\s[-\\x{2013}]\\s.*\\b(mix|remix|version|edit|extended|dub|instrumental|"
            "live|remaster|remastered|radio|club|single|album|original)\\b.*$"),
            QRegularExpression::CaseInsensitiveOption);
    t.remove(kVersionEnding);
    // "feat. X", "ft. X", "featuring X" up to the end.
    static const QRegularExpression kFeaturing(
            QStringLiteral("\\s(feat\\.?|ft\\.?|featuring)\\s.*$"),
            QRegularExpression::CaseInsensitiveOption);
    t.remove(kFeaturing);
    // A year tag at the end: "Freedom! '90".
    static const QRegularExpression kYearTag(QStringLiteral("\\s['\\x{2019}]\\d{2}\\s*$"));
    t.remove(kYearTag);
    t = withoutLeadingThe(tidy(t));
    // Some files carry the artist in the title too: "Big Pig - Breakaway!".
    if (!normalizedArtist.isEmpty() && t.startsWith(normalizedArtist + QChar(' '))) {
        t = t.mid(normalizedArtist.size() + 1);
    }
    return t;
}

// static
QString BridgeFinder::artistKey(const TrackFeatures& track) {
    return normalizeArtist(track.artist);
}

// static
QString BridgeFinder::nameKey(const TrackFeatures& track) {
    QString artist = track.artist;
    QString title = track.title;
    if (title.trimmed().isEmpty()) {
        // Not tagged: "Artist - Title" from the display name (or file name).
        const QString display = track.displayName;
        const int dash = display.indexOf(QStringLiteral(" - "));
        if (dash > 0) {
            artist = display.left(dash);
            title = display.mid(dash + 3);
        } else {
            title = display;
        }
    }
    if (artist.trimmed().isEmpty()) {
        // Untagged artist: "Lucretia My Reflection by Destroid".
        static const QRegularExpression kByArtist(QStringLiteral("\\sby\\s.+$"),
                QRegularExpression::CaseInsensitiveOption);
        title.remove(kByArtist);
    }
    return normalizeTitle(title, normalizeArtist(artist));
}

BridgeFinder::BridgeFinder(const MixScorer& scorer)
        : m_scorer(scorer) {
}

// static
std::optional<TrackFeatures> BridgeFinder::pickStart(const QVector<TrackFeatures>& candidates,
        const QSet<TrackId>& excludeIds,
        const QSet<QString>& excludeNames,
        bool calm,
        quint32 randomValue) {
    // An opener Auto DJ can beatmatch out of (steady grid, not a whole DJ
    // mix), unless there is none.
    QVector<const TrackFeatures*> usable;
    for (const bool strict : {true, false}) {
        for (const TrackFeatures& x : candidates) {
            if (x.hasKey() && x.hasBpm() && !excludeIds.contains(x.id) &&
                    !excludeNames.contains(nameKey(x)) &&
                    (!strict || (!x.gridUnsteady && x.durationSec <= kFillMaxSongSec))) {
                usable.append(&x);
            }
        }
        if (!usable.isEmpty()) {
            break;
        }
    }
    if (usable.isEmpty()) {
        return std::nullopt;
    }
    if (calm) {
        QVector<const TrackFeatures*> rated;
        for (const TrackFeatures* p : usable) {
            if (p->hasEnergy()) {
                rated.append(p);
            }
        }
        if (!rated.isEmpty()) {
            std::sort(rated.begin(), rated.end(), [](const auto* a, const auto* b) {
                return a->energy < b->energy;
            });
            // The calmest quarter (at least one track).
            rated.resize(std::max<qsizetype>(1, rated.size() / 4));
            usable = rated;
        }
    }
    return *usable[static_cast<qsizetype>(randomValue % static_cast<quint32>(usable.size()))];
}

QList<TrackFeatures> BridgeFinder::extend(const TrackFeatures& last,
        const QVector<TrackFeatures>& candidates,
        QSet<TrackId> excludeIds,
        QSet<QString> excludeNames,
        int count,
        bool avoidSameArtist,
        quint32 randomSeed,
        int keyMorphLimit,
        QList<int>* pKeyShifts) const {
    QList<TrackFeatures> chain;
    if (pKeyShifts) {
        pKeyShifts->clear();
    }
    std::mt19937 rng(randomSeed);
    TrackFeatures current = last;
    int sameKeyRun = 1; // tracks in a row in current's key, current included
    excludeIds.insert(last.id);
    if (!last.displayName.isEmpty()) {
        excludeNames.insert(nameKey(last));
    }
    const auto sameKey = [](const TrackFeatures& a, const TrackFeatures& b) {
        return a.hasKey() && a.camelotNumber == b.camelotNumber &&
                a.camelotMinor == b.camelotMinor;
    };
    struct Option {
        double cost;
        const TrackFeatures* pTrack;
        int keyShift; // key morph, semitones
    };
    while (static_cast<int>(chain.size()) < count) {
        QVector<Option> options;
        // First only tracks Auto DJ can beatmatch (steady beat grid); a
        // drifting one only when nothing else mixes.
        const auto collect = [&](bool steadyOnly) {
            for (const TrackFeatures& x : candidates) {
                if (!x.hasKey() || !x.hasBpm() || excludeIds.contains(x.id) ||
                        (!x.displayName.isEmpty() && excludeNames.contains(nameKey(x)))) {
                    continue;
                }
                if ((steadyOnly && x.gridUnsteady) || x.durationSec > kFillMaxSongSec) {
                    continue; // drifting grid (first pass), or a whole DJ mix
                }
                if (avoidSameArtist) {
                    const QString artist = artistKey(current);
                    if (!artist.isEmpty() && artist == artistKey(x)) {
                        continue;
                    }
                }
                MixScore s = m_scorer.score(current, x);
                int keyShift = 0;
                if (!MixScorer::clashLabel(s).isEmpty()) {
                    // Key morph: only in a beatmatched mix (steady grids,
                    // no tempo clash), and only if it makes the keys fit.
                    if (keyMorphLimit <= 0 || current.gridUnsteady || x.gridUnsteady ||
                            s.tempoCost >= MixScorer::kClashTempoCost) {
                        continue;
                    }
                    keyShift = MixScorer::keyMorphSemitones(current, x, keyMorphLimit);
                    if (keyShift == 0) {
                        continue;
                    }
                    s = m_scorer.score(current, MixScorer::shiftKey(x, keyShift));
                    if (!MixScorer::clashLabel(s).isEmpty()) {
                        continue;
                    }
                }
                double cost = s.total + genreCost(current.genre, x.genre) +
                        (x.gridUnsteady ? kUnsteadyGridCost : 0.0) +
                        kKeyMorphCost * std::abs(keyShift);
                if (sameKeyRun >= 2 &&
                        sameKey(current, keyShift ? MixScorer::shiftKey(x, keyShift) : x)) {
                    cost += kSameKeyRunCost;
                }
                // Close tempos beatmatch safely (half or double time counts
                // as close, as in the scorer).
                if (current.hasBpm()) {
                    const double ratio = std::fabs(std::log(x.bpm / current.bpm));
                    const double pct = 100.0 *
                            std::min({ratio,
                                    std::fabs(std::log(2.0 * x.bpm / current.bpm)),
                                    std::fabs(std::log(x.bpm / (2.0 * current.bpm)))});
                    cost += std::max(0.0, pct - kFillTempoComfortPct) * kFillTempoCostPerPct;
                }
                options.append(Option{cost, &x, keyShift});
            }
        };
        collect(true);
        if (options.isEmpty()) {
            collect(false);
        }
        if (options.isEmpty()) {
            break;
        }
        std::sort(options.begin(), options.end(), [](const Option& a, const Option& b) {
            return a.cost < b.cost;
        });
        int pool = 1;
        if (randomSeed != 0) {
            while (pool < options.size() && pool < kRandomPoolSize &&
                    options[pool].cost <= options.front().cost + kRandomCostMargin) {
                ++pool;
            }
        }
        const Option& best =
                options[static_cast<qsizetype>(std::uniform_int_distribution<int>(0, pool - 1)(rng))];
        const TrackFeatures* pBest = best.pTrack;
        // The next track must fit the key this one will really play in.
        const TrackFeatures heard =
                best.keyShift ? MixScorer::shiftKey(*pBest, best.keyShift) : *pBest;
        sameKeyRun = sameKey(current, heard) ? sameKeyRun + 1 : 1;
        chain.append(*pBest);
        if (pKeyShifts) {
            pKeyShifts->append(best.keyShift);
        }
        excludeIds.insert(pBest->id);
        if (!pBest->displayName.isEmpty()) {
            excludeNames.insert(nameKey(*pBest));
        }
        current = heard;
    }
    return chain;
}

QList<NextSuggestion> BridgeFinder::suggestNext(const TrackFeatures& now,
        const QVector<TrackFeatures>& candidates,
        const QSet<TrackId>& excludeIds,
        const QSet<QString>& excludeNames,
        int count,
        bool avoidSameArtist) const {
    QList<NextSuggestion> found;
    if (count <= 0) {
        return found;
    }
    const QString nowName = now.displayName.isEmpty() ? QString() : nameKey(now);
    const QString nowArtist = avoidSameArtist ? artistKey(now) : QString();
    for (const TrackFeatures& x : candidates) {
        if (!x.hasKey() || !x.hasBpm() || x.id == now.id || excludeIds.contains(x.id)) {
            continue;
        }
        if (!x.displayName.isEmpty()) {
            const QString name = nameKey(x);
            if (name == nowName || excludeNames.contains(name)) {
                continue; // another copy (or a cover) of a song already used
            }
        }
        if (!nowArtist.isEmpty() && nowArtist == artistKey(x)) {
            continue;
        }
        NextSuggestion s;
        s.score = m_scorer.score(now, x);
        if (!MixScorer::clashLabel(s.score).isEmpty()) {
            continue;
        }
        s.track = x;
        s.cost = s.score.total + genreCost(now.genre, x.genre) +
                (x.gridUnsteady ? kUnsteadyGridCost : 0.0);
        found.append(s);
    }
    std::sort(found.begin(), found.end(), [](const NextSuggestion& a, const NextSuggestion& b) {
        return a.cost < b.cost;
    });
    if (found.size() > count) {
        found.erase(found.begin() + count, found.end());
    }
    return found;
}

namespace {

/// Can this library track be a bridge at all?
bool usableBridge(const TrackFeatures& x,
        const TrackFeatures& from,
        const TrackFeatures& to,
        const QSet<TrackId>& excludeIds,
        const QSet<QString>& excludeNames) {
    // Only tracks we can judge (key and BPM known) and beatmatch (a bridge
    // with a drifting grid would not mix either).
    return x.hasKey() && x.hasBpm() && !x.gridUnsteady && !excludeIds.contains(x.id) &&
            !(x.id == from.id) && !(x.id == to.id) &&
            (x.displayName.isEmpty() || !excludeNames.contains(BridgeFinder::nameKey(x)));
}

double videoCost(const TrackFeatures& x, const TrackFeatures& from, const TrackFeatures& to) {
    return (from.isVideo || to.isVideo) && !x.isVideo ? BridgeFinder::kNoVideoBridgeCost : 0.0;
}

void sortAndCut(QList<BridgeSuggestion>* pFound, int maxResults) {
    std::sort(pFound->begin(), pFound->end(), [](const BridgeSuggestion& a, const BridgeSuggestion& b) {
        return a.cost < b.cost;
    });
    if (pFound->size() > maxResults) {
        *pFound = pFound->mid(0, maxResults);
    }
}

} // namespace

QList<BridgeSuggestion> BridgeFinder::find(const TrackFeatures& from,
        const TrackFeatures& to,
        const QVector<TrackFeatures>& candidates,
        const QSet<TrackId>& excludeIds,
        const QSet<QString>& excludeNames,
        int maxResults) const {
    QList<BridgeSuggestion> found;
    for (const TrackFeatures& x : candidates) {
        if (!usableBridge(x, from, to, excludeIds, excludeNames)) {
            continue;
        }
        BridgeSuggestion s;
        s.in = m_scorer.score(from, x);
        s.out = m_scorer.score(x, to);
        // A bridge must make both mixes smooth, or it is no bridge.
        if (!MixScorer::clashLabel(s.in).isEmpty() ||
                !MixScorer::clashLabel(s.out).isEmpty()) {
            continue;
        }
        s.track = x;
        s.cost = s.in.total + s.out.total + videoCost(x, from, to);
        found.append(s);
    }
    sortAndCut(&found, maxResults);
    return found;
}

QList<BridgeSuggestion> BridgeFinder::findPairs(const TrackFeatures& from,
        const TrackFeatures& to,
        const QVector<TrackFeatures>& candidates,
        const QSet<TrackId>& excludeIds,
        const QSet<QString>& excludeNames,
        int maxResults) const {
    // First steps (from -> x) and last steps (y -> to) that are smooth, the
    // cheapest few of each; then the pairs whose middle step is smooth too.
    constexpr int kMaxPerSide = 80;
    QList<std::pair<double, const TrackFeatures*>> firsts;
    QList<std::pair<double, const TrackFeatures*>> lasts;
    for (const TrackFeatures& x : candidates) {
        if (!usableBridge(x, from, to, excludeIds, excludeNames)) {
            continue;
        }
        const MixScore in = m_scorer.score(from, x);
        if (MixScorer::clashLabel(in).isEmpty()) {
            firsts.append(std::make_pair(in.total + videoCost(x, from, to), &x));
        }
        const MixScore out = m_scorer.score(x, to);
        if (MixScorer::clashLabel(out).isEmpty()) {
            lasts.append(std::make_pair(out.total + videoCost(x, from, to), &x));
        }
    }
    const auto byCost = [](const auto& a, const auto& b) {
        return a.first < b.first;
    };
    std::sort(firsts.begin(), firsts.end(), byCost);
    std::sort(lasts.begin(), lasts.end(), byCost);
    if (firsts.size() > kMaxPerSide) {
        firsts = firsts.mid(0, kMaxPerSide);
    }
    if (lasts.size() > kMaxPerSide) {
        lasts = lasts.mid(0, kMaxPerSide);
    }
    QList<BridgeSuggestion> found;
    for (const auto& [firstCost, pFirst] : firsts) {
        for (const auto& [lastCost, pSecond] : lasts) {
            if (pFirst->id == pSecond->id ||
                    (!pFirst->displayName.isEmpty() && !pSecond->displayName.isEmpty() &&
                            nameKey(*pFirst) == nameKey(*pSecond))) {
                continue; // not the same song twice
            }
            const MixScore mid = m_scorer.score(*pFirst, *pSecond);
            if (!MixScorer::clashLabel(mid).isEmpty()) {
                continue;
            }
            BridgeSuggestion s;
            s.track = *pFirst;
            s.second = *pSecond;
            s.isPair = true;
            s.in = m_scorer.score(from, *pFirst);
            s.out = m_scorer.score(*pSecond, to);
            s.cost = firstCost + mid.total + lastCost;
            found.append(s);
        }
    }
    sortAndCut(&found, maxResults);
    return found;
}
