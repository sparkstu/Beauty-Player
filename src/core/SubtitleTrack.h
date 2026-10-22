#pragma once
#include <QString>
#include <mutex>
#include <vector>

// Text subtitle model: SRT, WebVTT, ASS/SSA (dialogue text only) and embedded
// text subtitle streams (SRT/ASS/mov_text/... inside the container).
//
// Loading happens on the GUI thread when a file or track is picked; the render
// thread only reads, so everything is behind a small mutex.
class SubtitleTrack {
public:
    struct Cue {
        double start = 0.0;
        double end = 0.0;
        QString text;       // '\n' separated, override/HTML tags already stripped
        int alignment = 2;  // ASS numpad style: 1-3 bottom, 4-6 middle, 7-9 top
        int fontSize = 0;   // script pixels (ASS); 0 means "derive from the frame"
    };

    // One subtitle stream inside the container.
    struct EmbeddedTrack {
        int index = -1;      // AVStream index
        QString codec;       // e.g. "subrip", "ass", "mov_text"
        QString language;    // metadata "language", may be empty
        QString title;       // metadata "title", may be empty
        bool text = false;   // false for bitmap formats (PGS/DVD/DVB)
    };

    // |path| also selects the parser by extension; the content is sniffed when
    // the extension says nothing.
    bool loadFile(const QString& path, QString* err);
    bool loadText(const QString& text, const QString& name, QString* err);
    // Decodes a text subtitle stream out of |path|. This reads the whole
    // container, so it can take a moment on large files.
    bool loadEmbedded(const QString& path, int streamIndex, QString* err);
    void clear();

    // Enumerates the subtitle streams of |path|. Returns an empty vector when
    // the file cannot be opened or has no subtitle streams.
    static std::vector<EmbeddedTrack> embeddedTracks(const QString& path);

    bool empty() const;
    QString path() const;
    // Short human readable name: file name, or "内嵌 chi · ass".
    QString label() const;
    size_t cueCount() const;
    // Cues active at |t|, joined with '\n'; empty when nothing is on screen.
    QString textAt(double t) const;
    // Script resolution from ASS headers, 0 when unknown.
    int scriptWidth() const;
    int scriptHeight() const;

    // Removes {...} override blocks and <...> tags, expands \N / \n / \h.
    static QString stripTags(const QString& raw);
    // "00:01:02,500" / "1:02.5" -> seconds. Returns false when unparseable.
    static bool parseTimestamp(const QString& s, double* out);

private:
    bool parse(const QString& text, const QString& ext, QString* err);
    bool parseSrtLike(const QString& text, bool vtt, QString* err);
    bool parseAss(const QString& text, QString* err);

    mutable std::mutex mx_;
    std::vector<Cue> cues_;
    QString path_;
    QString label_;
    int scriptW_ = 0;
    int scriptH_ = 0;
};
