// Mari Paint — .psd 구현 (include/mari/psd/psd.hpp)
//
// 포맷 참고: Adobe Photoshop File Formats Specification (공개). 여기서 쓰는 부분만:
//   헤더 26B · 색 모드 데이터 · 이미지 리소스 · 레이어&마스크 정보(레이어 레코드 + 채널 데이터) · 합성 이미지.
// 레이어 순서: PSD 레코드는 **아래에서 위** — Mari 의 roots() 와 같다(인덱스 0 = 가장 아래).
// 그룹: 'lsct' 추가 정보. 폴더 시작은 "</Layer group>" 이름의 숨은 구분 레이어(type 3), 폴더 끝이 type 1/2 레코드.
//   → 레코드를 아래에서 위로 읽으면 구분 레이어(3)가 먼저, 폴더 레이어(1/2)가 나중에 온다.
#include <mari/psd/psd.hpp>

#include <mari/core/fs.hpp>
#include <mari/ora/image.hpp>
#include <mari/ora/ora.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>

namespace mari::psd {

namespace {

// ── 바이트 I/O ────────────────────────────────────────────────────────────

class Reader {
public:
    Reader(const u8* d, usize n) : d_(d), n_(n) {}
    [[nodiscard]] bool has(usize k) const { return pos_ + k <= n_; }
    [[nodiscard]] usize pos() const { return pos_; }
    void seek(usize p) { pos_ = std::min(p, n_); }
    void skip(usize k) { seek(pos_ + k); }
    u8 u8v() { return has(1) ? d_[pos_++] : (fail_ = true, u8{0}); }
    u16 rd16() { const u32 a = u8v(), b = u8v(); return static_cast<u16>((a << 8) | b); }
    u32 rd32() { const u32 a = rd16(), b = rd16(); return (a << 16) | b; }
    i32 i32v() { return static_cast<i32>(rd32()); }
    [[nodiscard]] bool failed() const { return fail_; }
    [[nodiscard]] const u8* ptr() const { return d_ + pos_; }
    std::string bytes(usize k) {
        if (!has(k)) { fail_ = true; return {}; }
        std::string s(reinterpret_cast<const char*>(d_ + pos_), k);
        pos_ += k;
        return s;
    }

private:
    const u8* d_;
    usize n_;
    usize pos_ = 0;
    bool fail_ = false;
};

class Writer {
public:
    void u8v(u8 v) { b_.push_back(v); }
    void w16(u16 v) { u8v(static_cast<u8>(v >> 8)); u8v(static_cast<u8>(v)); }
    void w32(u32 v) { w16(static_cast<u16>(v >> 16)); w16(static_cast<u16>(v)); }
    void i32v(i32 v) { w32(static_cast<u32>(v)); }
    void raw(const void* p, usize n) { const u8* q = static_cast<const u8*>(p); b_.insert(b_.end(), q, q + n); }
    void raw(const std::vector<u8>& v) { b_.insert(b_.end(), v.begin(), v.end()); }
    void str(const std::string& s) { raw(s.data(), s.size()); }
    void patchU32(usize at, u32 v) {
        b_[at] = static_cast<u8>(v >> 24); b_[at + 1] = static_cast<u8>(v >> 16);
        b_[at + 2] = static_cast<u8>(v >> 8); b_[at + 3] = static_cast<u8>(v);
    }
    [[nodiscard]] usize size() const { return b_.size(); }
    std::vector<u8>& bytes() { return b_; }

private:
    std::vector<u8> b_;
};

// ── PackBits ──────────────────────────────────────────────────────────────

bool unpackRow(Reader& r, usize len, u8* dst, usize width) {
    const usize end = r.pos() + len;
    usize x = 0;
    while (r.pos() < end && x < width) {
        const i8 n = static_cast<i8>(r.u8v());
        if (n >= 0) {
            const usize cnt = static_cast<usize>(n) + 1;
            for (usize i = 0; i < cnt && x < width; ++i) dst[x++] = r.u8v();
        } else if (n != -128) {
            const usize cnt = static_cast<usize>(-static_cast<int>(n)) + 1;
            const u8 v = r.u8v();
            for (usize i = 0; i < cnt && x < width; ++i) dst[x++] = v;
        }
    }
    while (x < width) dst[x++] = 0;
    r.seek(end);
    return !r.failed();
}

std::vector<u8> packRow(const u8* row, usize width) {
    std::vector<u8> out;
    usize x = 0;
    while (x < width) {
        usize run = 1;
        while (x + run < width && row[x + run] == row[x] && run < 128) ++run;
        if (run >= 2) {
            out.push_back(static_cast<u8>(static_cast<i8>(1 - static_cast<int>(run))));
            out.push_back(row[x]);
            x += run;
            continue;
        }
        usize lit = 1;
        while (x + lit < width && lit < 128 && !(x + lit + 1 < width && row[x + lit] == row[x + lit + 1])) ++lit;
        out.push_back(static_cast<u8>(lit - 1));
        out.insert(out.end(), row + x, row + x + lit);
        x += lit;
    }
    return out;
}

/// 채널 하나(높이 h, 너비 w)를 읽어 plane 에 넣는다. compression 0 Raw · 1 RLE.
bool readChannel(Reader& r, usize len, i32 w, i32 h, std::vector<u8>& plane) {
    const usize end = r.pos() + len;
    plane.assign(static_cast<usize>(w) * static_cast<usize>(h), 0);
    if (w <= 0 || h <= 0 || len < 2) { r.seek(end); return true; }
    const u16 comp = r.rd16();
    if (comp == 0) {
        const usize need = plane.size();
        if (!r.has(need)) return false;
        std::memcpy(plane.data(), r.ptr(), need);
        r.seek(end);
        return true;
    }
    if (comp == 1) {
        std::vector<u16> lens(static_cast<usize>(h));
        for (auto& l : lens) l = r.rd16();
        for (i32 y = 0; y < h; ++y)
            if (!unpackRow(r, lens[static_cast<usize>(y)], plane.data() + static_cast<usize>(y) * w, static_cast<usize>(w))) return false;
        r.seek(end);
        return true;
    }
    return false; // ZIP 압축은 안 한다
}

void writeChannel(Writer& w, const std::vector<u8>& plane, i32 width, i32 height, bool rle) {
    if (width <= 0 || height <= 0) { w.w16(0); return; }
    if (!rle) {
        w.w16(0);
        w.raw(plane);
        return;
    }
    w.w16(1);
    std::vector<std::vector<u8>> rows;
    rows.reserve(static_cast<usize>(height));
    for (i32 y = 0; y < height; ++y) rows.push_back(packRow(plane.data() + static_cast<usize>(y) * width, static_cast<usize>(width)));
    for (const auto& r : rows) w.w16(static_cast<u16>(std::min<usize>(r.size(), 65535)));
    for (const auto& r : rows) w.raw(r);
}

// ── 합성 모드 표 ──────────────────────────────────────────────────────────

struct BlendRow { const char* key; BlendMode mode; };
const BlendRow kBlends[] = {
    {"norm", BlendMode::Normal},   {"mul ", BlendMode::Multiply},  {"scrn", BlendMode::Screen},
    {"over", BlendMode::Overlay},  {"dark", BlendMode::Darken},    {"lite", BlendMode::Lighten},
    {"div ", BlendMode::ColorDodge}, {"idiv", BlendMode::ColorBurn}, {"hLit", BlendMode::HardLight},
    {"sLit", BlendMode::SoftLight}, {"diff", BlendMode::Difference}, {"smud", BlendMode::Exclusion},
    {"hue ", BlendMode::Hue},      {"sat ", BlendMode::Saturation}, {"colr", BlendMode::Color},
    {"lum ", BlendMode::Luminosity}, {"lddg", BlendMode::Add},      {"fsub", BlendMode::Subtract},
};

std::string pascal(Reader& r, usize pad) {
    const u8 n = r.u8v();
    std::string s = r.bytes(n);
    const usize total = 1 + n;
    if (total % pad) r.skip(pad - total % pad);
    return s;
}

std::string utf16beToUtf8(const std::string& raw) {
    std::string out;
    for (usize i = 0; i + 1 < raw.size(); i += 2) {
        u32 c = (static_cast<u8>(raw[i]) << 8) | static_cast<u8>(raw[i + 1]);
        if (c >= 0xD800 && c <= 0xDBFF && i + 3 < raw.size()) {
            const u32 lo = (static_cast<u8>(raw[i + 2]) << 8) | static_cast<u8>(raw[i + 3]);
            c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
            i += 2;
        }
        if (c < 0x80) out.push_back(static_cast<char>(c));
        else if (c < 0x800) { out.push_back(static_cast<char>(0xC0 | (c >> 6))); out.push_back(static_cast<char>(0x80 | (c & 0x3F))); }
        else if (c < 0x10000) { out.push_back(static_cast<char>(0xE0 | (c >> 12))); out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F))); out.push_back(static_cast<char>(0x80 | (c & 0x3F))); }
        else { out.push_back(static_cast<char>(0xF0 | (c >> 18))); out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F))); out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F))); out.push_back(static_cast<char>(0x80 | (c & 0x3F))); }
    }
    return out;
}

std::string utf8ToUtf16be(const std::string& s) {
    std::string out;
    for (usize i = 0; i < s.size();) {
        const u8 c = static_cast<u8>(s[i]);
        u32 cp;
        usize len;
        if (c < 0x80) { cp = c; len = 1; }
        else if ((c >> 5) == 6) { cp = c & 0x1F; len = 2; }
        else if ((c >> 4) == 14) { cp = c & 0x0F; len = 3; }
        else { cp = c & 0x07; len = 4; }
        for (usize k = 1; k < len && i + k < s.size(); ++k) cp = (cp << 6) | (static_cast<u8>(s[i + k]) & 0x3F);
        i += len;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            const u32 hi = 0xD800 + (cp >> 10), lo = 0xDC00 + (cp & 0x3FF);
            out.push_back(static_cast<char>(hi >> 8)); out.push_back(static_cast<char>(hi));
            out.push_back(static_cast<char>(lo >> 8)); out.push_back(static_cast<char>(lo));
        } else {
            out.push_back(static_cast<char>(cp >> 8)); out.push_back(static_cast<char>(cp));
        }
    }
    return out;
}

// ── 읽기 ─────────────────────────────────────────────────────────────────

struct LayerRec {
    i32 top = 0, left = 0, bottom = 0, right = 0;
    struct Chan { i16 id; u32 len; };
    std::vector<Chan> chans;
    char blend[4] = {'n', 'o', 'r', 'm'};
    u8 opacity = 255;
    bool clipping = false;
    bool hidden = false;
    std::string name;
    int section = 0; ///< lsct: 0 없음 · 1 열린 폴더 · 2 닫힌 폴더 · 3 구분자
    // 마스크
    bool hasMask = false;
    i32 mTop = 0, mLeft = 0, mBottom = 0, mRight = 0;
    u8 mDefault = 0;
};

/// 레이어/마스크 사각형이 말이 되는가: 좌표 ±kMaxDim 안, 너비·높이 kMaxDim 이하(음수는 빈 것으로 본다).
constexpr i32 kMaxDim = 30000;
bool rectSane(i32 top, i32 left, i32 bottom, i32 right) noexcept {
    if (top < -kMaxDim || left < -kMaxDim || bottom > 2 * kMaxDim || right > 2 * kMaxDim) return false;
    const i64 w = static_cast<i64>(right) - left, h = static_cast<i64>(bottom) - top;
    return w <= kMaxDim && h <= kMaxDim;
}

} // namespace

const char* blendKeyOf(BlendMode m) noexcept {
    for (const BlendRow& r : kBlends) if (r.mode == m) return r.key;
    return "norm";
}

bool blendFromKey(const char key[4], BlendMode& out) noexcept {
    for (const BlendRow& r : kBlends) {
        if (std::memcmp(r.key, key, 4) == 0) { out = r.mode; return true; }
    }
    return false;
}

Result<Document> loadFromMemory(const u8* data, usize size) {
    Reader r(data, size);
    if (r.bytes(4) != "8BPS") return Err("PSD 시그니처가 아니다", ErrorCode::ParseError);
    const u16 version = r.rd16();
    if (version != 1) return Err("PSB(버전 2)는 아직 읽지 않는다", ErrorCode::Unsupported);
    r.skip(6);
    const u16 channels = r.rd16();
    const i32 height = r.i32v();
    const i32 width = r.i32v();
    const u16 depth = r.rd16();
    const u16 colorMode = r.rd16();
    if (r.failed()) return Err("PSD 머리말이 잘렸다", ErrorCode::ParseError);
    if (depth != 8) return Err("8비트 PSD 만 읽는다(이 파일은 " + std::to_string(depth) + "비트)", ErrorCode::Unsupported);
    if (colorMode != 3) return Err("RGB PSD 만 읽는다(색 모드 " + std::to_string(colorMode) + ")", ErrorCode::Unsupported);
    if (width <= 0 || height <= 0 || width > 30000 || height > 30000) return Err("캔버스 크기가 범위 밖이다", ErrorCode::ParseError);
    (void)channels;

    Document doc;
    Result<LayerTreePtr> made = makeLayerTree(Size{width, height});
    if (!made.ok()) return made.error();
    doc.tree = std::move(made).value();
    LayerTree& tree = *doc.tree;

    const u32 colorLen = r.rd32(); r.skip(colorLen);
    const u32 resLen = r.rd32(); r.skip(resLen);
    const u32 lmLen = r.rd32();
    const usize lmEnd = r.pos() + lmLen;
    if (r.failed()) return Err("PSD 섹션 길이가 잘렸다", ErrorCode::ParseError);

    std::vector<LayerRec> recs;
    if (lmLen > 0) {
        const u32 layerInfoLen = r.rd32();
        const usize layerInfoEnd = r.pos() + layerInfoLen;
        if (layerInfoLen > 0) {
            const i16 count = static_cast<i16>(r.rd16());
            const usize n = static_cast<usize>(count < 0 ? -count : count); // 음수 = 첫 알파가 합성 투명도
            for (usize i = 0; i < n && !r.failed(); ++i) {
                LayerRec L;
                L.top = r.i32v(); L.left = r.i32v(); L.bottom = r.i32v(); L.right = r.i32v();
                // 🔴 손상된 파일이 w×h 를 수십 GB 로 만들 수 있다 — 캔버스와 같은 한도로 자른다(bad_alloc 대신 Err).
                if (!rectSane(L.top, L.left, L.bottom, L.right))
                    return Err("레이어 사각형이 범위 밖이다(손상된 PSD)", ErrorCode::ParseError);
                const u16 nch = r.rd16();
                for (u16 c = 0; c < nch; ++c) {
                    LayerRec::Chan ch;
                    ch.id = static_cast<i16>(r.rd16());
                    ch.len = r.rd32();
                    L.chans.push_back(ch);
                }
                if (r.bytes(4) != "8BIM") return Err("레이어 레코드 시그니처가 아니다", ErrorCode::ParseError);
                const std::string bk = r.bytes(4);
                std::memcpy(L.blend, bk.data(), 4);
                L.opacity = r.u8v();
                L.clipping = r.u8v() != 0;
                const u8 flags = r.u8v();
                L.hidden = (flags & 0x02) != 0;
                r.skip(1);
                const u32 extraLen = r.rd32();
                const usize extraEnd = r.pos() + extraLen;
                // 마스크
                const u32 maskLen = r.rd32();
                if (maskLen >= 20) {
                    const usize mEnd = r.pos() + maskLen;
                    L.hasMask = true;
                    L.mTop = r.i32v(); L.mLeft = r.i32v(); L.mBottom = r.i32v(); L.mRight = r.i32v();
                    if (!rectSane(L.mTop, L.mLeft, L.mBottom, L.mRight))
                        return Err("레이어 마스크 사각형이 범위 밖이다(손상된 PSD)", ErrorCode::ParseError);
                    L.mDefault = r.u8v();
                    r.seek(mEnd);
                } else {
                    r.skip(maskLen);
                }
                const u32 blendRangesLen = r.rd32(); r.skip(blendRangesLen);
                L.name = pascal(r, 4);
                // 추가 정보 블록들
                while (r.pos() + 12 <= extraEnd && !r.failed()) {
                    const std::string sig = r.bytes(4);
                    if (sig != "8BIM" && sig != "8B64") break;
                    const std::string key = r.bytes(4);
                    const u32 len = r.rd32();
                    const usize bEnd = r.pos() + len + (len % 2);
                    if (key == "luni") {
                        const u32 chars = r.rd32();
                        L.name = utf16beToUtf8(r.bytes(static_cast<usize>(chars) * 2));
                    } else if (key == "lsct") {
                        L.section = static_cast<int>(r.rd32());
                    }
                    r.seek(bEnd);
                }
                r.seek(extraEnd);
                recs.push_back(std::move(L));
            }
            // 채널 데이터: 레코드 순서대로.
            // 그룹 스택: 아래→위로 읽으므로 구분자(3)가 먼저 나온다 → 그때 그룹을 만들고 push, 폴더 레코드(1/2)에서 pop.
            std::vector<LayerId> stack;
            const auto parentOf = [&] { return stack.empty() ? kInvalidLayerId : stack.back(); };
            for (LayerRec& L : recs) {
                const i32 w = L.right - L.left, h = L.bottom - L.top;
                std::vector<u8> R, G, B, A, M;
                bool hasA = false;
                for (const LayerRec::Chan& ch : L.chans) {
                    std::vector<u8>* dst = nullptr;
                    i32 cw = w, chh = h;
                    if (ch.id == 0) dst = &R;
                    else if (ch.id == 1) dst = &G;
                    else if (ch.id == 2) dst = &B;
                    else if (ch.id == -1) { dst = &A; hasA = true; }
                    else if (ch.id == -2 && L.hasMask) { dst = &M; cw = L.mRight - L.mLeft; chh = L.mBottom - L.mTop; }
                    if (dst == nullptr) { r.skip(ch.len); continue; }
                    if (!readChannel(r, ch.len, cw, chh, *dst)) {
                        doc.warnings.push_back("'" + L.name + "' 채널 " + std::to_string(ch.id) + " 을 읽지 못했다(압축 방식 미지원)");
                        r.skip(0);
                    }
                }
                if (L.section == 3) {
                    // 폴더 시작(숨은 구분자). 이름은 폴더 레코드에서 온다.
                    Result<LayerPtr> g = tree.addGroup("그룹", parentOf(), -1);
                    if (!g.ok()) return g.error();
                    stack.push_back(g.value()->id());
                    continue;
                }
                if (L.section == 1 || L.section == 2) {
                    if (stack.empty()) { doc.warnings.push_back("짝이 없는 폴더 레코드: " + L.name); continue; }
                    const LayerPtr g = tree.find(stack.back());
                    stack.pop_back();
                    g->setName(L.name);
                    g->setOpacity(static_cast<f32>(L.opacity) / 255.0f);
                    g->setVisible(!L.hidden);
                    BlendMode bm = BlendMode::Normal;
                    if (blendFromKey(L.blend, bm) && bm != BlendMode::Normal) g->setBlendMode(bm);
                    continue;
                }
                Result<LayerPtr> made2 = tree.addRaster(L.name.empty() ? "레이어" : L.name, parentOf(), -1);
                if (!made2.ok()) return made2.error();
                const LayerPtr layer = made2.value();
                layer->setOpacity(static_cast<f32>(L.opacity) / 255.0f);
                layer->setVisible(!L.hidden);
                layer->setClipToBelow(L.clipping);
                BlendMode bm = BlendMode::Normal;
                if (!blendFromKey(L.blend, bm)) doc.warnings.push_back("'" + L.name + "' 의 합성 모드 '" + std::string(L.blend, 4) + "' 은 대응이 없어 보통으로 뒀다");
                layer->setBlendMode(bm);
                if (w > 0 && h > 0 && !R.empty()) {
                    ora::Image8 img = ora::Image8::make(w, h);
                    for (usize i = 0; i < static_cast<usize>(w) * static_cast<usize>(h); ++i) {
                        img.pixels[i * 4] = R[i];
                        img.pixels[i * 4 + 1] = G.size() > i ? G[i] : R[i];
                        img.pixels[i * 4 + 2] = B.size() > i ? B[i] : R[i];
                        img.pixels[i * 4 + 3] = hasA && A.size() > i ? A[i] : 255;
                    }
                    const Result<void> wr = ora::writeRegion(*layer->tiles(), Rect{L.left, L.top, w, h}, img);
                    if (!wr.ok()) return wr.error();
                }
                if (L.hasMask && !M.empty()) {
                    const i32 mw = L.mRight - L.mLeft, mh = L.mBottom - L.mTop;
                    Result<TileMapPtr> mask = makeTileMap(PixelFormat::Gray8);
                    if (!mask.ok()) return mask.error();
                    // 마스크 밖 기본값(mDefault)을 캔버스 전체에 깔고 사각형을 덮어쓴다.
                    for (i32 ty = 0; ty <= tileIndexFor(height - 1); ++ty)
                        for (i32 tx = 0; tx <= tileIndexFor(width - 1); ++tx) {
                            Result<TilePtr> t = mask.value()->writable(TileCoord{tx, ty});
                            if (!t.ok()) return t.error();
                            u8* p = t.value()->mutablePixels();
                            for (i32 y = 0; y < kTileSize; ++y)
                                for (i32 x = 0; x < kTileSize; ++x) {
                                    const i32 cx = tileOrigin(tx) + x, cy = tileOrigin(ty) + y;
                                    u8 v = L.mDefault;
                                    if (cx >= L.mLeft && cx < L.mLeft + mw && cy >= L.mTop && cy < L.mTop + mh)
                                        v = M[static_cast<usize>(cy - L.mTop) * mw + static_cast<usize>(cx - L.mLeft)];
                                    p[static_cast<usize>(y) * t.value()->stride() + static_cast<usize>(x)] = v;
                                }
                        }
                    layer->setMask(mask.value());
                }
            }
            if (!stack.empty()) doc.warnings.push_back("닫히지 않은 폴더가 있다");
        }
        r.seek(layerInfoEnd);
    }
    r.seek(lmEnd);
    if (recs.empty()) {
        // 레이어가 없으면 합성 이미지를 한 장으로.
        const u16 comp = r.rd16();
        const usize n = static_cast<usize>(width) * static_cast<usize>(height);
        std::vector<std::vector<u8>> planes(channels);
        if (comp == 0) {
            for (u16 c = 0; c < channels; ++c) { planes[c].assign(n, 0); if (r.has(n)) { std::memcpy(planes[c].data(), r.ptr(), n); r.skip(n); } }
        } else if (comp == 1) {
            std::vector<u16> lens(static_cast<usize>(height) * channels);
            for (auto& l : lens) l = r.rd16();
            for (u16 c = 0; c < channels; ++c) {
                planes[c].assign(n, 0);
                for (i32 y = 0; y < height; ++y)
                    unpackRow(r, lens[static_cast<usize>(c) * height + y], planes[c].data() + static_cast<usize>(y) * width, static_cast<usize>(width));
            }
        } else {
            return Err("합성 이미지 압축 방식을 모른다", ErrorCode::Unsupported);
        }
        Result<LayerPtr> made2 = tree.addRaster("배경", kInvalidLayerId, -1);
        if (!made2.ok()) return made2.error();
        ora::Image8 img = ora::Image8::make(width, height);
        for (usize i = 0; i < n; ++i) {
            img.pixels[i * 4] = planes.size() > 0 ? planes[0][i] : 0;
            img.pixels[i * 4 + 1] = planes.size() > 1 ? planes[1][i] : img.pixels[i * 4];
            img.pixels[i * 4 + 2] = planes.size() > 2 ? planes[2][i] : img.pixels[i * 4];
            img.pixels[i * 4 + 3] = planes.size() > 3 ? planes[3][i] : 255;
        }
        const Result<void> wr = ora::writeRegion(*made2.value()->tiles(), Rect{0, 0, width, height}, img);
        if (!wr.ok()) return wr.error();
    }
    if (!tree.roots().empty()) (void)tree.setActiveLayer(tree.roots().back()->id());
    return Ok(std::move(doc));
}

Result<Document> load(const std::string& path) {
    Result<std::vector<u8>> bytes = ora::readFileBytes(path);
    if (!bytes.ok()) return bytes.error();
    return loadFromMemory(bytes.value().data(), bytes.value().size());
}

// ── 쓰기 ─────────────────────────────────────────────────────────────────

namespace {

struct OutLayer {
    const Layer* layer = nullptr;
    int section = 0;          ///< 0 래스터 · 1 폴더(닫는 레코드) · 3 구분자
    std::string name;
};

/// Mari 트리(아래→위, 자식은 부모 안) → PSD 레코드 순서(아래→위): 구분자, 자식들…, 폴더.
void flattenForPsd(const std::vector<LayerPtr>& list, std::vector<OutLayer>& out) {
    for (const LayerPtr& l : list) {
        if (l->kind() == LayerKind::Group) {
            out.push_back({l.get(), 3, "</Layer group>"});
            flattenForPsd(l->children(), out);
            out.push_back({l.get(), 1, l->name()});
        } else {
            out.push_back({l.get(), 0, l->name()});
        }
    }
}

void writeLayerRecord(Writer& w, const OutLayer& ol, const Rect& r, const std::vector<std::pair<i16, u32>>& chans,
                      bool hasMask, const Rect& maskRect) {
    w.i32v(r.y); w.i32v(r.x); w.i32v(r.y + r.height); w.i32v(r.x + r.width);
    w.w16(static_cast<u16>(chans.size()));
    for (const auto& [id, len] : chans) { w.w16(static_cast<u16>(id)); w.w32(len); }
    w.str("8BIM");
    w.str(ol.section == 0 ? blendKeyOf(ol.layer->blendMode()) : (ol.section == 1 ? blendKeyOf(ol.layer->blendMode()) : "pass"));
    w.u8v(ol.section == 3 ? 255 : static_cast<u8>(std::lround(ol.layer->opacity() * 255.0f)));
    w.u8v(ol.section == 0 && ol.layer->clipToBelow() ? 1 : 0);
    u8 flags = 0x08; // bit3: bit4 유효
    if (!ol.layer->visible() && ol.section != 3) flags |= 0x02;
    if (ol.section == 3) flags |= 0x10 | 0x02; // 구분자는 숨김
    w.u8v(flags);
    w.u8v(0);
    const usize extraLenAt = w.size();
    w.w32(0);
    const usize extraStart = w.size();
    // 마스크
    if (hasMask) {
        w.w32(20);
        w.i32v(maskRect.y); w.i32v(maskRect.x); w.i32v(maskRect.y + maskRect.height); w.i32v(maskRect.x + maskRect.width);
        w.u8v(0);  // 기본값(사각형 밖 = 가림)
        w.u8v(0);  // flags
        w.w16(0);  // padding
    } else {
        w.w32(0);
    }
    w.w32(0); // blending ranges
    // 파스칼 이름(4 정렬)
    std::string pn = ol.name.substr(0, 255);
    w.u8v(static_cast<u8>(pn.size()));
    w.str(pn);
    const usize padTo = 4 - ((1 + pn.size()) % 4);
    if (padTo != 4) for (usize i = 0; i < padTo; ++i) w.u8v(0);
    // luni
    {
        const std::string u16s = utf8ToUtf16be(ol.name);
        w.str("8BIM"); w.str("luni");
        const u32 len = 4 + static_cast<u32>(u16s.size());
        w.w32(len + (len % 2));
        w.w32(static_cast<u32>(u16s.size() / 2));
        w.str(u16s);
        if (len % 2) w.u8v(0);
    }
    if (ol.section != 0) {
        w.str("8BIM"); w.str("lsct"); w.w32(12);
        w.w32(static_cast<u32>(ol.section));
        w.str("8BIM"); w.str(ol.section == 3 ? "pass" : blendKeyOf(ol.layer->blendMode()));
    }
    w.patchU32(extraLenAt, static_cast<u32>(w.size() - extraStart));
}

} // namespace

Result<std::vector<u8>> saveToMemory(const LayerTree& tree, const SaveOptions& opts) {
    const Size cs = tree.canvasSize();
    if (cs.width <= 0 || cs.height <= 0 || cs.width > 30000 || cs.height > 30000)
        return Err("PSD v1 은 30000px 까지다", ErrorCode::Unsupported);
    Writer w;
    w.str("8BPS"); w.w16(1); for (int i = 0; i < 6; ++i) w.u8v(0);
    w.w16(4); w.w32(static_cast<u32>(cs.height)); w.w32(static_cast<u32>(cs.width)); w.w16(8); w.w16(3);
    w.w32(0); // color mode data
    w.w32(0); // image resources
    const usize lmLenAt = w.size(); w.w32(0);
    const usize lmStart = w.size();
    const usize liLenAt = w.size(); w.w32(0);
    const usize liStart = w.size();

    std::vector<OutLayer> outs;
    flattenForPsd(tree.roots(), outs);
    w.w16(static_cast<u16>(outs.size()));
    // 채널 데이터를 먼저 만들어 길이를 알아야 레코드를 쓸 수 있다.
    struct Prepared { Rect r; std::vector<std::vector<u8>> chanData; std::vector<i16> ids; bool hasMask = false; Rect maskRect; };
    std::vector<Prepared> prep(outs.size());
    for (usize i = 0; i < outs.size(); ++i) {
        const OutLayer& ol = outs[i];
        Prepared& P = prep[i];
        if (ol.section != 0 || ol.layer->tiles() == nullptr) {
            P.r = Rect{0, 0, 0, 0};
            for (i16 id : {i16{-1}, i16{0}, i16{1}, i16{2}}) { Writer cw; cw.w16(0); P.chanData.push_back(cw.bytes()); P.ids.push_back(id); }
            continue;
        }
        Rect b = ol.layer->tiles()->bounds().intersected(Rect{0, 0, cs.width, cs.height});
        if (b.isEmpty()) b = Rect{0, 0, 0, 0};
        P.r = b;
        std::vector<u8> R, G, B, A;
        if (!b.isEmpty()) {
            Result<ora::Image8> img = ora::readRegion(*ol.layer->tiles(), b);
            if (!img.ok()) return img.error();
            const usize n = static_cast<usize>(b.width) * static_cast<usize>(b.height);
            R.resize(n); G.resize(n); B.resize(n); A.resize(n);
            for (usize k = 0; k < n; ++k) {
                R[k] = img.value().pixels[k * 4]; G[k] = img.value().pixels[k * 4 + 1];
                B[k] = img.value().pixels[k * 4 + 2]; A[k] = img.value().pixels[k * 4 + 3];
            }
        }
        for (const auto& [id, plane] : {std::pair<i16, const std::vector<u8>*>{-1, &A}, {0, &R}, {1, &G}, {2, &B}}) {
            Writer cw;
            writeChannel(cw, *plane, b.width, b.height, opts.rle);
            P.chanData.push_back(cw.bytes());
            P.ids.push_back(id);
        }
        if (const TileMap* mask = ol.layer->mask()) {
            // 마스크는 캔버스 전체 사각형으로 쓴다(없는 타일 = 0 = 가림 규약과 일치).
            P.hasMask = true;
            P.maskRect = Rect{0, 0, cs.width, cs.height};
            std::vector<u8> M(static_cast<usize>(cs.width) * static_cast<usize>(cs.height), 0);
            for (i32 y = 0; y < cs.height; ++y)
                for (i32 x = 0; x < cs.width; ++x) {
                    const ConstTilePtr t = mask->at(TileCoord{tileIndexFor(x), tileIndexFor(y)});
                    if (!t) continue;
                    M[static_cast<usize>(y) * cs.width + x] = t->pixels()[static_cast<usize>(y - tileOrigin(tileIndexFor(y))) * t->stride() + static_cast<usize>(x - tileOrigin(tileIndexFor(x)))];
                }
            Writer cw;
            writeChannel(cw, M, cs.width, cs.height, opts.rle);
            P.chanData.push_back(cw.bytes());
            P.ids.push_back(-2);
        }
    }
    for (usize i = 0; i < outs.size(); ++i) {
        std::vector<std::pair<i16, u32>> chans;
        for (usize c = 0; c < prep[i].ids.size(); ++c) chans.emplace_back(prep[i].ids[c], static_cast<u32>(prep[i].chanData[c].size()));
        writeLayerRecord(w, outs[i], prep[i].r, chans, prep[i].hasMask, prep[i].maskRect);
    }
    for (const Prepared& P : prep) for (const auto& cd : P.chanData) w.raw(cd);
    if ((w.size() - liStart) % 2) w.u8v(0);
    w.patchU32(liLenAt, static_cast<u32>(w.size() - liStart));
    w.w32(0); // global layer mask info
    w.patchU32(lmLenAt, static_cast<u32>(w.size() - lmStart));

    // 합성 이미지(RGBA planar, RLE).
    std::vector<u8> comp(static_cast<usize>(cs.width) * static_cast<usize>(cs.height) * 4u);
    const Result<void> f = tree.flatten(Rect{0, 0, cs.width, cs.height}, comp.data(), static_cast<usize>(cs.width) * 4u);
    if (!f.ok()) return f.error();
    const usize n = static_cast<usize>(cs.width) * static_cast<usize>(cs.height);
    std::vector<std::vector<u8>> planes(4, std::vector<u8>(n));
    for (usize k = 0; k < n; ++k) for (int c = 0; c < 4; ++c) planes[static_cast<usize>(c)][k] = comp[k * 4 + static_cast<usize>(c)];
    if (opts.rle) {
        w.w16(1);
        std::vector<std::vector<u8>> rows;
        rows.reserve(4u * static_cast<usize>(cs.height));
        for (int c = 0; c < 4; ++c)
            for (i32 y = 0; y < cs.height; ++y)
                rows.push_back(packRow(planes[static_cast<usize>(c)].data() + static_cast<usize>(y) * cs.width, static_cast<usize>(cs.width)));
        for (const auto& row : rows) w.w16(static_cast<u16>(std::min<usize>(row.size(), 65535)));
        for (const auto& row : rows) w.raw(row);
    } else {
        w.w16(0);
        for (int c = 0; c < 4; ++c) w.raw(planes[static_cast<usize>(c)]);
    }
    return Ok(std::move(w.bytes()));
}

Result<void> save(const LayerTree& tree, const std::string& path, const SaveOptions& opts) {
    Result<std::vector<u8>> bytes = saveToMemory(tree, opts);
    if (!bytes.ok()) return bytes.error();
    return ora::writeFileBytes(path, bytes.value().data(), bytes.value().size());
}

} // namespace mari::psd
