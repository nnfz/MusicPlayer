#include <iostream>
#include <fstream>
#include <string>
#include <regex>

using namespace std;

int main() {
    ifstream in("src/core/TrackItem.cpp");
    if (!in) {
        cerr << "Failed to open input file" << endl;
        return 1;
    }
    string content((istreambuf_iterator<char>(in)), (istreambuf_iterator<char>()));
    in.close();

    // Find start of the block
    size_t startIdx = content.find("bool isLikelyCoverImageCodecId");
    if (startIdx == string::npos) {
        cerr << "Could not find start" << endl;
        return 1;
    }

    // Find end of extractEmbeddedCoverArt
    size_t midIdx = content.find("QImage extractEmbeddedCoverArt");
    if (midIdx == string::npos) {
        cerr << "Could not find mid" << endl;
        return 1;
    }

    size_t endIdx = midIdx;
    int braceCount = 0;
    bool inFunction = false;
    for (size_t i = midIdx; i < content.length(); ++i) {
        if (content[i] == '{') {
            inFunction = true;
            braceCount++;
        } else if (content[i] == '}') {
            braceCount--;
            if (inFunction && braceCount == 0) {
                endIdx = i + 1;
                break;
            }
        }
    }

    if (endIdx <= midIdx) {
        cerr << "Could not find end of function" << endl;
        return 1;
    }

    string bassImpl = R"(
void applyBassMetadata(const QString &filePath, TrackMetadata &metadata)
{
#ifdef MUSICPLAYER_HAS_BASS
    HSTREAM stream = BASS_StreamCreateFile(FALSE, filePath.utf16(), 0, 0, BASS_STREAM_DECODE | BASS_UNICODE);
    if (!stream) return;

    qint64 lenBytes = BASS_ChannelGetLength(stream, BASS_POS_BYTE);
    double lenSecs = BASS_ChannelBytes2Seconds(stream, lenBytes);
    if (lenSecs > 0) metadata.duration = static_cast<qint64>(lenSecs * 1000.0);

    BASS_CHANNELINFO info;
    if (BASS_ChannelGetInfo(stream, &info)) {
        metadata.sampleRate = info.freq;
        if (lenSecs > 0) {
            QFileInfo fi(filePath);
            metadata.bitrate = static_cast<int>((fi.size() * 8) / lenSecs);
        }
    }

    auto getTag = [&](DWORD tagType) -> bool {
        const char *tags = BASS_ChannelGetTags(stream, tagType);
        if (tags) {
            if (tagType == BASS_TAG_ID3V2) {
                return false; // ID3v2 is complex, skip for now
            } else if (tagType == BASS_TAG_ID3) {
                TAG_ID3 *id3 = (TAG_ID3*)tags;
                if (id3->id[0] == 'T' && id3->id[1] == 'A' && id3->id[2] == 'G') {
                    metadata.title = QString::fromLocal8Bit(id3->title, 30).trimmed();
                    metadata.artist = QString::fromLocal8Bit(id3->artist, 30).trimmed();
                    metadata.album = QString::fromLocal8Bit(id3->album, 30).trimmed();
                    metadata.year = QString::fromLocal8Bit(id3->year, 4).trimmed();
                    if (id3->track > 0) metadata.trackNumber = id3->track;
                    return true;
                }
            } else if (tagType == BASS_TAG_OGG || tagType == BASS_TAG_FLAC_CUE) {
                while (*tags) {
                    QString tag = QString::fromUtf8(tags);
                    int eqIdx = tag.indexOf('=');
                    if (eqIdx > 0) {
                        QString key = tag.left(eqIdx).toLower();
                        QString val = tag.mid(eqIdx + 1);
                        if (key == "title" && metadata.title.isEmpty()) metadata.title = val;
                        else if ((key == "artist" || key == "albumartist") && metadata.artist.isEmpty()) metadata.artist = val;
                        else if (key == "album" && metadata.album.isEmpty()) metadata.album = val;
                        else if (key == "date" && metadata.year.isEmpty()) metadata.year = val;
                        else if (key == "tracknumber" && metadata.trackNumber == 0) metadata.trackNumber = val.toInt();
                        else if (key == "genre" && metadata.genre.isEmpty()) metadata.genre = val;
                    }
                    tags += tag.length() + 1;
                }
                return true;
            }
        }
        return false;
    };

    if (!getTag(BASS_TAG_OGG)) {
        if (!getTag(BASS_TAG_FLAC_CUE)) {
             getTag(BASS_TAG_ID3);
        }
    }

    BASS_StreamFree(stream);
#endif
}

QImage extractCoverArt(const QString &filePath) {
    Q_UNUSED(filePath);
    return QImage();
}

QImage extractEmbeddedCoverArt(const QString &filePath) {
    Q_UNUSED(filePath);
    return QImage();
}
)";

    content.replace(startIdx, endIdx - startIdx, bassImpl);

    // Replace applyFfmpegMetadata invocations with applyBassMetadata
    size_t pos = 0;
    while ((pos = content.find("applyFfmpegMetadata", pos)) != string::npos) {
        content.replace(pos, 19, "applyBassMetadata");
        pos += 17;
    }

    // Strip #ifdef MUSICPLAYER_HAS_FFMPEG blocks
    regex ffmpeg_regex("#ifdef MUSICPLAYER_HAS_FFMPEG[\\s\\S]*?#endif");
    content = regex_replace(content, ffmpeg_regex, "");

    ofstream out("src/core/TrackItem.cpp");
    out << content;
    out.close();

    cout << "Success!" << endl;
    return 0;
}