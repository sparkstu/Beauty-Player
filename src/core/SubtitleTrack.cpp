#include "SubtitleTrack.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QStringDecoder>
#include <QStringList>
#include <algorithm>
#include <cmath>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libavutil/mathematics.h>
}

#include "Log.h"

namespace {

QString decodeBytes(const QByteArray& raw, bool* clean) {
    *clean = true;
    if (raw.startsWith("\xEF\xBB\xBF"))
        return QString::fromUtf8(raw.mid(3));
    if (raw.startsWith("\xFF\xFE")) {
        QStringDecoder d(QStringDecoder::Utf16LE);
        return d(raw.mid(2));
    }
    if (raw.startsWith("\xFE\xFF")) {
        QStringDecoder d(QStringDecoder::Utf16BE);
        return d(raw.mid(2));
    }

    QStringDecoder utf8(QStringDecoder::Utf8);
    const QString s = utf8(raw);
    if (!utf8.hasError()) return s;
    *clean = false;

    // Not UTF-8. Prefer an explicitly requested code page, then the system one
    // (CP936/GBK on a Chinese Windows), which covers most legacy .srt files.
    const QByteArray want = qgetenv("PLAY_SUB_ENC");
    if (!want.isEmpty()) {
        if (const auto enc = QStringConverter::encodingForName(want.constData())) {
            QStringDecoder d(*enc);
            const QString t = d(raw);
            if (!d.hasError()) {
                *clean = true;
                return t;
            }
        } else {
            LOGW("subtitle: unsupported PLAY_SUB_ENC=%s", want.constData());
        }
    }
    return QString::fromLocal8Bit(raw);
}

QString normalized(const QString& text) {
    QString s = text;
    s.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    s.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    return s;
}

QString unescapeHtml(const QString& s) {
    QString out = s;
    out.replace(QStringLiteral("&nbsp;"), QStringLiteral(" "));
    out.replace(QStringLiteral("&amp;"), QStringLiteral("&"));
    out.replace(QStringLiteral("&lt;"), QStringLiteral("<"));
    out.replace(QStringLiteral("&gt;"), QStringLiteral(">"));
    out.replace(QStringLiteral("&quot;"), QStringLiteral("\""));
    return out;
}

// SRT indexes are optional and VTT cues may carry "align:..." after the end
// time, so the timing line is parsed by splitting on the arrow.
bool parseTimingLine(const QString& line, double* start, double* end) {
    const int arrow = line.indexOf(QStringLiteral("-->"));
    if (arrow < 0) return false;
    const QString a = line.left(arrow).trimmed();
    QString b = line.mid(arrow + 3).trimmed();
    const int space = b.indexOf(QLatin1Char(' '));
    if (space > 0) b = b.left(space);  // drop cue settings
    return SubtitleTrack::parseTimestamp(a, start) && SubtitleTrack::parseTimestamp(b, end);
}

// Text based subtitle codecs can be decoded into strings; bitmap ones (PGS,
// DVD, DVB, XSUB) would need an image compositor, which we do not have.
bool isTextSubtitle(AVCodecID id) {
    switch (id) {
    case AV_CODEC_ID_SUBRIP:
    case AV_CODEC_ID_ASS:
    case AV_CODEC_ID_SSA:
    case AV_CODEC_ID_WEBVTT:
    case AV_CODEC_ID_MOV_TEXT:
    case AV_CODEC_ID_TEXT:
    case AV_CODEC_ID_MICRODVD:
    case AV_CODEC_ID_SAMI:
    case AV_CODEC_ID_JACOSUB:
    case AV_CODEC_ID_SUBVIEWER:
    case AV_CODEC_ID_SUBVIEWER1:
    case AV_CODEC_ID_VPLAYER:
    case AV_CODEC_ID_REALTEXT:
    case AV_CODEC_ID_STL:
    case AV_CODEC_ID_PJS:
    case AV_CODEC_ID_MPL2:
    case AV_CODEC_ID_SRT:
    case AV_CODEC_ID_TTML:
    case AV_CODEC_ID_HDMV_TEXT_SUBTITLE:
    case AV_CODEC_ID_ARIB_CAPTION:
    case AV_CODEC_ID_EIA_608:
        return true;
    default:
        return false;
    }
}

// ASS decoder output is "layer,start,end,style,name,marginL,marginR,marginV,
// effect,text"; everything before the 9th comma is metadata. The generic text
// decoder (embedded SRT/mov_text) emits the same wrapper with one field less,
// so fall back to the 8th comma before giving up.
QString assDialogueText(const QString& ass) {
    int commas = 0;
    int eighth = -1;
    for (int i = 0; i < ass.size(); ++i) {
        if (ass[i] != QLatin1Char(',')) continue;
        ++commas;
        if (commas == 8) eighth = i;
        if (commas == 9) return ass.mid(i + 1);
    }
    return eighth >= 0 ? ass.mid(eighth + 1) : ass;
}

}  // namespace

bool SubtitleTrack::parseTimestamp(const QString& s, double* out) {
    const QString t = s.trimmed();
    if (t.isEmpty()) return false;
    QStringList parts = t.split(QLatin1Char(':'));
    if (parts.isEmpty() || parts.size() > 3) return false;

    double frac = 0.0;
    QString secPart = parts.last();
    for (int i = 0; i < secPart.size(); ++i) {
        if (secPart[i] == QLatin1Char('.') || secPart[i] == QLatin1Char(',')) {
            const QString f = secPart.mid(i + 1);
            secPart = secPart.left(i);
            bool ok = false;
            double v = f.toDouble(&ok);
            if (!ok) return false;
            // "5" means 500 ms, "05" 50 ms, "500" 500 ms.
            frac = v / std::pow(10.0, double(f.size()));
            break;
        }
    }
    bool okSec = false;
    const long secs = secPart.toLong(&okSec);
    if (!okSec) return false;

    long h = 0, m = 0;
    if (parts.size() == 3) {
        bool okH = false, okM = false;
        h = parts[0].toLong(&okH);
        m = parts[1].toLong(&okM);
        if (!okH || !okM) return false;
    } else if (parts.size() == 2) {
        bool okM = false;
        m = parts[0].toLong(&okM);
        if (!okM) return false;
    }
    *out = double(h) * 3600.0 + double(m) * 60.0 + double(secs) + frac;
    return true;
}

QString SubtitleTrack::stripTags(const QString& raw) {
    QString out;
    out.reserve(raw.size());
    for (int i = 0; i < raw.size(); ++i) {
        const QChar c = raw[i];
        if (c == QLatin1Char('{')) {
            const int close = raw.indexOf(QLatin1Char('}'), i + 1);
            if (close < 0) break;
            i = close;
        } else if (c == QLatin1Char('<')) {
            const int close = raw.indexOf(QLatin1Char('>'), i + 1);
            if (close < 0) break;
            i = close;
        } else if (c == QLatin1Char('\\') && i + 1 < raw.size()) {
            const QChar n = raw[i + 1];
            if (n == QLatin1Char('N') || n == QLatin1Char('n')) {
                out += QLatin1Char('\n');
                ++i;
            } else if (n == QLatin1Char('h')) {
                out += QLatin1Char(' ');
                ++i;
            } else if (n == QLatin1Char('\\')) {
                out += QLatin1Char('\\');
                ++i;
            } else {
                out += c;
            }
        } else {
            out += c;
        }
    }
    out = unescapeHtml(out);

    QStringList lines = out.split(QLatin1Char('\n'));
    for (QString& l : lines) l = l.trimmed();
    while (!lines.isEmpty() && lines.first().isEmpty()) lines.removeFirst();
    while (!lines.isEmpty() && lines.last().isEmpty()) lines.removeLast();
    return lines.join(QLatin1Char('\n'));
}

bool SubtitleTrack::loadFile(const QString& path, QString* err) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (err) *err = QStringLiteral("无法打开字幕文件: %1").arg(path);
        return false;
    }
    const QByteArray bytes = f.readAll();
    f.close();

    bool clean = true;
    const QString text = decodeBytes(bytes, &clean);
    if (!clean)
        LOGW("subtitle: %s is not valid UTF-8, decoded with the fallback code page",
             qPrintable(QFileInfo(path).fileName()));

    const QString ext = QFileInfo(path).suffix().toLower();
    if (!parse(normalized(text), ext, err)) return false;
    {
        std::lock_guard<std::mutex> lock(mx_);
        path_ = path;
        label_ = QFileInfo(path).fileName();
    }
    LOGI("subtitle: loaded %zu cues from %s", cueCount(), qPrintable(path));
    return true;
}

bool SubtitleTrack::loadText(const QString& text, const QString& name, QString* err) {
    const QString ext = QFileInfo(name).suffix().toLower();
    if (!parse(normalized(text), ext, err)) return false;
    {
        std::lock_guard<std::mutex> lock(mx_);
        path_ = name;
        label_ = QFileInfo(name).fileName();
    }
    return true;
}

std::vector<SubtitleTrack::EmbeddedTrack> SubtitleTrack::embeddedTracks(const QString& path) {
    std::vector<EmbeddedTrack> out;
    AVFormatContext* fmt = nullptr;
    const QByteArray p = QFile::encodeName(path);
    if (avformat_open_input(&fmt, p.constData(), nullptr, nullptr) < 0) return out;
    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        avformat_close_input(&fmt);
        return out;
    }
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        const AVStream* st = fmt->streams[i];
        if (st->codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE) continue;

        EmbeddedTrack t;
        t.index = int(i);
        const AVCodecDescriptor* d = avcodec_descriptor_get(st->codecpar->codec_id);
        t.codec = d ? QString::fromUtf8(d->name) : QStringLiteral("?");
        if (const AVDictionaryEntry* e = av_dict_get(st->metadata, "language", nullptr, 0))
            t.language = QString::fromUtf8(e->value);
        if (const AVDictionaryEntry* e = av_dict_get(st->metadata, "title", nullptr, 0))
            t.title = QString::fromUtf8(e->value);
        if (const AVDictionaryEntry* e = av_dict_get(st->metadata, "handler_name", nullptr, 0)) {
            if (t.title.isEmpty()) t.title = QString::fromUtf8(e->value);
        }
        t.text = isTextSubtitle(st->codecpar->codec_id);
        out.push_back(t);
    }
    avformat_close_input(&fmt);
    return out;
}

bool SubtitleTrack::loadEmbedded(const QString& path, int streamIndex, QString* err) {
    QElapsedTimer timer;
    timer.start();

    AVFormatContext* fmt = nullptr;
    const QByteArray p = QFile::encodeName(path);
    if (avformat_open_input(&fmt, p.constData(), nullptr, nullptr) < 0) {
        if (err) *err = QStringLiteral("无法打开视频文件: %1").arg(path);
        return false;
    }
    if (avformat_find_stream_info(fmt, nullptr) < 0 || streamIndex < 0 ||
        streamIndex >= int(fmt->nb_streams)) {
        avformat_close_input(&fmt);
        if (err) *err = QStringLiteral("找不到内嵌字幕轨");
        return false;
    }

    AVStream* st = fmt->streams[streamIndex];
    if (st->codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE) {
        avformat_close_input(&fmt);
        if (err) *err = QStringLiteral("该流不是字幕轨");
        return false;
    }
    if (!isTextSubtitle(st->codecpar->codec_id)) {
        avformat_close_input(&fmt);
        if (err) *err = QStringLiteral("位图字幕（PGS/DVD）暂不支持");
        return false;
    }

    const AVCodec* dec = avcodec_find_decoder(st->codecpar->codec_id);
    AVCodecContext* cc = dec ? avcodec_alloc_context3(dec) : nullptr;
    if (!cc || avcodec_parameters_to_context(cc, st->codecpar) < 0 ||
        avcodec_open2(cc, dec, nullptr) < 0) {
        avcodec_free_context(&cc);
        avformat_close_input(&fmt);
        if (err) *err = QStringLiteral("无法打开字幕解码器");
        return false;
    }
    cc->pkt_timebase = st->time_base;
    const AVCodecID codecId = st->codecpar->codec_id;  // |st| dies with |fmt|

    // Decoded text, plus the language/title so the UI can label the track.
    std::vector<Cue> cues;
    QString language, title;
    if (const AVDictionaryEntry* e = av_dict_get(st->metadata, "language", nullptr, 0))
        language = QString::fromUtf8(e->value);
    if (const AVDictionaryEntry* e = av_dict_get(st->metadata, "title", nullptr, 0))
        title = QString::fromUtf8(e->value);

    AVPacket* pkt = av_packet_alloc();
    while (av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index == streamIndex) {
            AVSubtitle sub{};
            int got = 0;
            if (avcodec_decode_subtitle2(cc, &sub, &got, pkt) >= 0 && got) {
                QStringList parts;
                for (unsigned r = 0; r < sub.num_rects; ++r) {
                    const AVSubtitleRect* rect = sub.rects[r];
                    if (!rect) continue;
                    if (rect->type == SUBTITLE_ASS && rect->ass)
                        parts << assDialogueText(QString::fromUtf8(rect->ass));
                    else if (rect->text)
                        parts << QString::fromUtf8(rect->text);
                }
                Cue cue;
                const double start =
                    pkt->pts == AV_NOPTS_VALUE ? 0.0 : pkt->pts * av_q2d(st->time_base);
                cue.start = start;
                cue.end = start + (sub.end_display_time > 0 ? sub.end_display_time / 1000.0 : 0.0);
                cue.text = stripTags(parts.join(QLatin1Char('\n')));
                if (!cue.text.isEmpty()) cues.push_back(cue);
                avsubtitle_free(&sub);
            }
        }
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    avcodec_free_context(&cc);
    avformat_close_input(&fmt);

    std::sort(cues.begin(), cues.end(),
              [](const Cue& a, const Cue& b) { return a.start < b.start; });
    // Demuxers often leave the duration at zero; bound those cues by the next
    // cue so they do not linger on screen.
    for (size_t i = 0; i < cues.size(); ++i) {
        if (cues[i].end > cues[i].start) continue;
        cues[i].end = i + 1 < cues.size() && cues[i + 1].start > cues[i].start
                          ? cues[i + 1].start
                          : cues[i].start + 3.0;
    }
    cues.erase(std::unique(cues.begin(), cues.end(),
                           [](const Cue& a, const Cue& b) {
                               return std::abs(a.start - b.start) < 0.001 && a.text == b.text;
                           }),
               cues.end());

    if (cues.empty()) {
        if (err) *err = QStringLiteral("这条字幕轨没有可显示的文字");
        return false;
    }

    QString name = title;
    if (name.isEmpty()) name = language.isEmpty() ? QStringLiteral("未知语言") : language;
    const AVCodecDescriptor* d = avcodec_descriptor_get(codecId);
    {
        std::lock_guard<std::mutex> lock(mx_);
        cues_ = std::move(cues);
        path_ = QStringLiteral("%1#%2").arg(path).arg(streamIndex);
        label_ = d ? QStringLiteral("内嵌 %1 · %2").arg(name, QString::fromUtf8(d->name))
                   : QStringLiteral("内嵌 %1").arg(name);
        scriptW_ = 0;
        scriptH_ = 0;
    }
    LOGI("subtitle: decode embedded stream %d (%s) -> %zu cues in %lld ms", streamIndex,
         qPrintable(label()), cueCount(), static_cast<long long>(timer.elapsed()));
    return true;
}

bool SubtitleTrack::parse(const QString& text, const QString& ext, QString* err) {
    const QString head = text.left(2048);
    bool ass = ext == QLatin1String("ass") || ext == QLatin1String("ssa") ||
               head.contains(QLatin1String("[Script Info]")) ||
               head.contains(QLatin1String("[V4+ Styles]"));
    bool vtt = ext == QLatin1String("vtt") || head.startsWith(QLatin1String("WEBVTT"));

    bool ok = ass ? parseAss(text, err) : parseSrtLike(text, vtt, err);
    if (!ok) return false;
    if (cues_.empty()) {
        if (err) *err = QStringLiteral("字幕文件里没有可用的字幕行");
        return false;
    }
    std::sort(cues_.begin(), cues_.end(),
              [](const Cue& a, const Cue& b) { return a.start < b.start; });
    return true;
}

bool SubtitleTrack::parseSrtLike(const QString& text, bool /*vtt*/, QString* err) {
    std::vector<Cue> out;
    const QStringList lines = text.split(QLatin1Char('\n'));

    int i = 0;
    while (i < lines.size()) {
        const QString line = lines[i];
        double start = 0.0, end = 0.0;
        const bool isTiming = parseTimingLine(line, &start, &end);
        if (!isTiming) {
            ++i;
            continue;
        }

        Cue cue;
        cue.start = start;
        cue.end = end;

        // Alignment hint from an ASS override that some SRT files carry.
        const QString& timing = line;
        if (timing.contains(QLatin1String("{\\an"))) {
            const int p = timing.indexOf(QLatin1String("{\\an")) + 4;
            if (p < timing.size()) cue.alignment = timing[p].digitValue();
        }

        ++i;
        QStringList body;
        while (i < lines.size()) {
            QString l = lines[i];
            if (l.trimmed().isEmpty()) {
                ++i;
                break;
            }
            if (parseTimingLine(l, &start, &end)) break;  // malformed block
            body << l;
            ++i;
        }
        cue.text = stripTags(body.join(QLatin1Char('\n')));
        if (!cue.text.isEmpty() && cue.end > cue.start) out.push_back(cue);
    }

    if (out.empty()) {
        if (err) *err = QStringLiteral("没有解析到 SRT/VTT 字幕行");
        return false;
    }
    std::lock_guard<std::mutex> lock(mx_);
    cues_ = std::move(out);
    return true;
}

bool SubtitleTrack::parseAss(const QString& text, QString* err) {
    struct Style {
        int fontSize = 0;
        int alignment = 2;
    };
    QMap<QString, Style> styles;

    std::vector<Cue> out;
    QString section;
    QMap<QString, int> styleCols;
    QMap<QString, int> eventCols;
    int scriptW = 0, scriptH = 0;

    const QStringList lines = text.split(QLatin1Char('\n'));
    for (QString line : lines) {
        line = line.trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char(';'))) continue;

        if (line.startsWith(QLatin1Char('['))) {
            section = line.toLower();
            continue;
        }

        if (section.contains(QLatin1String("script info"))) {
            if (line.startsWith(QLatin1String("PlayResX:")))
                scriptW = line.mid(9).trimmed().toInt();
            else if (line.startsWith(QLatin1String("PlayResY:")))
                scriptH = line.mid(9).trimmed().toInt();
            continue;
        }

        if (section.contains(QLatin1String("styles"))) {
            if (line.startsWith(QLatin1String("Format:"), Qt::CaseInsensitive)) {
                styleCols.clear();
                const QStringList cols = line.mid(7).split(QLatin1Char(','));
                for (int c = 0; c < cols.size(); ++c)
                    styleCols.insert(cols[c].trimmed().toLower(), c);
                continue;
            }
            if (line.startsWith(QLatin1String("Style:"), Qt::CaseInsensitive)) {
                const QStringList f = line.mid(6).split(QLatin1Char(','));
                Style st;
                if (styleCols.contains(QStringLiteral("fontsize")))
                    st.fontSize = int(f.value(styleCols.value(QStringLiteral("fontsize"))).toDouble());
                if (styleCols.contains(QStringLiteral("alignment")))
                    st.alignment = f.value(styleCols.value(QStringLiteral("alignment"))).trimmed().toInt();
                if (st.alignment <= 0) st.alignment = 2;
                if (st.fontSize <= 0) st.fontSize = 48;
                styles.insert(f.value(styleCols.value(QStringLiteral("name"))).trimmed(), st);
            }
            continue;
        }

        if (section.contains(QLatin1String("events"))) {
            if (line.startsWith(QLatin1String("Format:"), Qt::CaseInsensitive)) {
                eventCols.clear();
                const QStringList cols = line.mid(7).split(QLatin1Char(','));
                for (int c = 0; c < cols.size(); ++c)
                    eventCols.insert(cols[c].trimmed().toLower(), c);
                continue;
            }
            if (!line.startsWith(QLatin1String("Dialogue:"), Qt::CaseInsensitive)) continue;

            const int textCol = eventCols.value(QStringLiteral("text"), -1);
            const QStringList f = line.mid(9).split(QLatin1Char(','));
            if (textCol < 0 || f.size() <= textCol) continue;

            Cue cue;
            if (!parseTimestamp(f.value(eventCols.value(QStringLiteral("start"))).trimmed(), &cue.start))
                continue;
            if (!parseTimestamp(f.value(eventCols.value(QStringLiteral("end"))).trimmed(), &cue.end))
                continue;

            QString rawText = f.mid(textCol).join(QLatin1Char(','));
            const QString styleName = f.value(eventCols.value(QStringLiteral("style"))).trimmed();
            const Style st = styles.value(styleName);
            cue.fontSize = st.fontSize;
            cue.alignment = st.alignment;

            // Inline \an overrides the style alignment.
            int an = rawText.indexOf(QLatin1String("\\an"));
            if (an >= 0 && an + 3 < rawText.size() && rawText[an + 3].isDigit())
                cue.alignment = rawText[an + 3].digitValue();

            cue.text = stripTags(rawText);
            if (!cue.text.isEmpty() && cue.end > cue.start) out.push_back(cue);
        }
    }

    if (out.empty()) {
        if (err) *err = QStringLiteral("没有解析到 ASS/SSA 对白行");
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mx_);
        cues_ = std::move(out);
        scriptW_ = scriptW;
        scriptH_ = scriptH;
    }
    return true;
}

void SubtitleTrack::clear() {
    std::lock_guard<std::mutex> lock(mx_);
    cues_.clear();
    path_.clear();
    label_.clear();
    scriptW_ = scriptH_ = 0;
}

bool SubtitleTrack::empty() const {
    std::lock_guard<std::mutex> lock(mx_);
    return cues_.empty();
}

QString SubtitleTrack::path() const {
    std::lock_guard<std::mutex> lock(mx_);
    return path_;
}

QString SubtitleTrack::label() const {
    std::lock_guard<std::mutex> lock(mx_);
    return label_;
}

size_t SubtitleTrack::cueCount() const {
    std::lock_guard<std::mutex> lock(mx_);
    return cues_.size();
}

int SubtitleTrack::scriptWidth() const {
    std::lock_guard<std::mutex> lock(mx_);
    return scriptW_;
}

int SubtitleTrack::scriptHeight() const {
    std::lock_guard<std::mutex> lock(mx_);
    return scriptH_;
}

QString SubtitleTrack::textAt(double t) const {
    std::lock_guard<std::mutex> lock(mx_);
    if (cues_.empty()) return {};

    // Cues are sorted by start; walk back a little to cover overlapping lines
    // (ASS commonly stacks two speakers).
    auto it = std::upper_bound(cues_.begin(), cues_.end(), t,
                               [](double v, const Cue& c) { return v < c.start; });
    int guard = 0;
    QStringList active;
    while (it != cues_.begin() && guard++ < 16) {
        --it;
        if (it->end <= t) {
            if (it->start < t - 30.0) break;  // far behind, stop scanning
            continue;
        }
        if (it->start <= t) active.prepend(it->text);
    }
    return active.join(QLatin1Char('\n'));
}
