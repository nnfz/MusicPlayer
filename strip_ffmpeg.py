import re

with open("src/core/TrackItem.cpp", "r", encoding="utf-8") as f:
    content = f.read()

# Replace the block from `bool isLikelyCoverImageCodecId` to the end of `extractEmbeddedCoverArt` with BASS implementation.

bass_impl = """
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
        // BASS doesn't directly expose bitrate in CHANNELINFO, but we can compute it
        if (lenSecs > 0) {
            // Rough bitrate: file size * 8 / lenSecs
            QFileInfo fi(filePath);
            metadata.bitrate = static_cast<int>((fi.size() * 8) / lenSecs);
        }
    }

    auto getTag = [&](DWORD tagType) -> bool {
        const char *tags = BASS_ChannelGetTags(stream, tagType);
        if (tags) {
            if (tagType == BASS_TAG_ID3V2) {
                // Not parsing raw ID3v2 right now, just fallback
                return false;
            } else if (tagType == BASS_TAG_ID3) {
                // ID3v1 is fixed length, 128 bytes
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
                // Vorbis comments are a series of null-terminated strings, ending with an empty string
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
                    tags += strlen(tags) + 1;
                }
                return true;
            }
        }
        return false;
    };

    if (!getTag(BASS_TAG_OGG)) {
        if (!getTag(BASS_TAG_FLAC_CUE)) { // Usually FLAC has OGG tags too
             getTag(BASS_TAG_ID3);
        }
    }

    BASS_StreamFree(stream);
#endif
}

QImage extractCoverArt(const QString &filePath) {
    return QImage();
}

QImage extractEmbeddedCoverArt(const QString &filePath) {
    return QImage();
}

"""

# Regex to find everything from `bool isLikelyCoverImageCodecId` down to the end of `extractEmbeddedCoverArt`
pattern = r"bool isLikelyCoverImageCodecId.*?QImage extractEmbeddedCoverArt[^{]*\{.*?^\}"
# We'll use a more robust way to match the block: find the first index and the last index.

start_idx = content.find("bool isLikelyCoverImageCodecId")
if start_idx != -1:
    end_str = "QImage extractEmbeddedCoverArt(const QString &path)"
    mid_idx = content.find(end_str)
    if mid_idx != -1:
        # Find the end of this function
        brace_count = 0
        in_function = False
        end_idx = mid_idx
        for i in range(mid_idx, len(content)):
            if content[i] == '{':
                in_function = True
                brace_count += 1
            elif content[i] == '}':
                brace_count -= 1
                if in_function and brace_count == 0:
                    end_idx = i + 1
                    break
        
        if end_idx > mid_idx:
            content = content[:start_idx] + bass_impl + content[end_idx:]

content = content.replace("applyFfmpegMetadata", "applyBassMetadata")

# Remove MUSICPLAYER_HAS_FFMPEG block entirely from TrackItem.cpp
content = re.sub(r'#ifdef MUSICPLAYER_HAS_FFMPEG.*?#endif', '', content, flags=re.DOTALL)

with open("src/core/TrackItem.cpp", "w", encoding="utf-8") as f:
    f.write(content)
