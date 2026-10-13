/*
 * id3scan.cpp - see id3scan.h.
 */

#include "id3scan.h"
#include <SD.h>

// ---------------------------------------------------------------------------
// Text decoding.  ID3v2 text frames start with an encoding byte:
//   0 = ISO-8859-1, 1 = UTF-16 with BOM, 2 = UTF-16BE, 3 = UTF-8.
// ID3v1 is ISO-8859-1 with no marker.

static void appendUtf8(String &out, uint32_t cp) {
    if (cp < 0x80) {
        out += (char)cp;
    } else if (cp < 0x800) {
        out += (char)(0xC0 | (cp >> 6));
        out += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    } else {
        out += (char)(0xF0 | (cp >> 18));
        out += (char)(0x80 | ((cp >> 12) & 0x3F));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
}

static String latin1ToUtf8(const uint8_t *p, size_t n) {
    String out;
    for (size_t i = 0; i < n && p[i]; i++) appendUtf8(out, p[i]);
    return out;
}

static String utf16ToUtf8(const uint8_t *p, size_t n, bool bigEndian) {
    String out;
    for (size_t i = 0; i + 1 < n; i += 2) {
        uint32_t cu = bigEndian ? (p[i] << 8) | p[i + 1] : (p[i + 1] << 8) | p[i];
        if (!cu) break;
        if (cu >= 0xD800 && cu < 0xDC00 && i + 3 < n) {         // surrogate pair
            uint32_t lo = bigEndian ? (p[i + 2] << 8) | p[i + 3] : (p[i + 3] << 8) | p[i + 2];
            cu = 0x10000 + ((cu - 0xD800) << 10) + (lo - 0xDC00);
            i += 2;
        }
        appendUtf8(out, cu);
    }
    return out;
}

static String decodeText(const uint8_t *p, size_t n) {
    if (n < 1) return String();
    uint8_t enc = p[0];
    p++; n--;
    switch (enc) {
    case 0:  return latin1ToUtf8(p, n);
    case 1:
        if (n >= 2 && p[0] == 0xFF && p[1] == 0xFE) return utf16ToUtf8(p + 2, n - 2, false);
        if (n >= 2 && p[0] == 0xFE && p[1] == 0xFF) return utf16ToUtf8(p + 2, n - 2, true);
        return utf16ToUtf8(p, n, false);
    case 2:  return utf16ToUtf8(p, n, true);
    case 3: {
        String out;
        for (size_t i = 0; i < n && p[i]; i++) out += (char)p[i];
        return out;
    }
    default: return String();
    }
}

static String trimmed(String s) {
    s.trim();
    return s;
}

// ---------------------------------------------------------------------------
// ID3v2

static uint32_t syncsafe(const uint8_t *b) {
    return ((uint32_t)(b[0] & 0x7F) << 21) | ((uint32_t)(b[1] & 0x7F) << 14) |
           ((uint32_t)(b[2] & 0x7F) << 7)  |  (uint32_t)(b[3] & 0x7F);
}
static uint32_t be32(const uint8_t *b) {
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
}

static bool readV2(File &f, String &title, String &artist) {
    uint8_t hdr[10];
    if (f.read(hdr, 10) != 10) return false;
    if (memcmp(hdr, "ID3", 3) != 0) return false;
    uint8_t ver = hdr[3];
    if (ver < 2 || ver > 4) return false;
    bool unsync = hdr[5] & 0x80;
    bool exthdr = ver >= 3 && (hdr[5] & 0x40);
    uint32_t tagEnd = 10 + syncsafe(hdr + 6);

    // Whole-tag unsynchronisation shifts every frame size; not worth
    // handling for a menu label.  Fall back to ID3v1 / the filename.
    if (unsync && ver < 4) return false;

    uint32_t pos = 10;
    if (exthdr) {
        uint8_t eh[4];
        if (f.read(eh, 4) != 4) return false;
        uint32_t ehSize = (ver == 4) ? syncsafe(eh) : be32(eh) + 4;
        pos += ehSize;
        f.seek(pos);
    }

    bool found = false;
    while (pos + 6 <= tagEnd) {
        char id[5] = {0};
        uint32_t size;
        uint32_t frameHdr = (ver == 2) ? 6 : 10;
        uint8_t fh[10];
        if (f.read(fh, frameHdr) != frameHdr) break;
        if (fh[0] == 0) break;                      // padding
        memcpy(id, fh, ver == 2 ? 3 : 4);
        if (ver == 2)      size = ((uint32_t)fh[3] << 16) | ((uint32_t)fh[4] << 8) | fh[5];
        else if (ver == 3) size = be32(fh + 4);
        else               size = syncsafe(fh + 4);
        pos += frameHdr;
        if (size == 0 || pos + size > tagEnd) break;

        bool isTitle  = (ver == 2) ? !strcmp(id, "TT2") : !strcmp(id, "TIT2");
        bool isArtist = (ver == 2) ? !strcmp(id, "TP1") : !strcmp(id, "TPE1");
        if ((isTitle || isArtist) && size < 1024) {
            uint8_t buf[1024];
            if (f.read(buf, size) != size) break;
            String s = trimmed(decodeText(buf, size));
            if (s.length()) {
                (isTitle ? title : artist) = s;
                found = true;
            }
            pos += size;
        } else {
            pos += size;
            f.seek(pos);
        }
        if (title.length() && artist.length()) break;
    }
    return found;
}

// ---------------------------------------------------------------------------
// ID3v1: the last 128 bytes of the file.

static bool readV1(File &f, String &title, String &artist) {
    if (f.size() < 128) return false;
    uint8_t tag[128];
    f.seek(f.size() - 128);
    if (f.read(tag, 128) != 128) return false;
    if (memcmp(tag, "TAG", 3) != 0) return false;
    bool found = false;
    if (!title.length()) {
        title = trimmed(latin1ToUtf8(tag + 3, 30));
        found |= title.length() > 0;
    }
    if (!artist.length()) {
        artist = trimmed(latin1ToUtf8(tag + 33, 30));
        found |= artist.length() > 0;
    }
    return found;
}

bool id3_read(const char *path, String &title, String &artist) {
    title = artist = String();
    File f = SD.open(path);
    if (!f) return false;
    bool found = readV2(f, title, artist);
    if (!title.length() || !artist.length())
        found |= readV1(f, title, artist);
    f.close();
    return found;
}
