// Mari Paint — .ora 읽기/쓰기 구현. 선언은 include/mari/ora/ora.hpp.
//
// ── 순서 규약이 뒤집힌다는 점을 기억할 것 ─────────────────────────────────
//   Mari  : children()[0] 이 **가장 아래**  (core/layer.hpp)
//   .ora  : stack.xml 의 **첫 자식이 가장 위** (OpenRaster 스펙)
// 그래서 쓸 때도 읽을 때도 한 번씩 뒤집는다. 이걸 빠뜨리면 그림이 뒤집혀 보인다.
#include <mari/ora/ora.hpp>
#include <mari/core/fs.hpp>

#include <mari/ora/composite_op.hpp>
#include <mari/ora/image.hpp>
#include <mari/ora/xml.hpp>
#include <mari/ora/zip.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace mari::ora {
namespace {

/// 불투명도처럼 사람이 읽을 실수 문자열. 꼬리 0을 떼어 "1", "0.5" 처럼 만든다.
std::string formatFloat(f32 v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.6f", static_cast<double>(v));
    std::string s(buf);
    const usize dot = s.find('.');
    if (dot != std::string::npos) {
        usize end = s.size();
        while (end > dot + 1 && s[end - 1] == '0')
            --end;
        if (end == dot + 1)
            end = dot;
        s.resize(end);
    }
    return s;
}

f32 parseFloat(const std::string* s, f32 fallback) {
    if (s == nullptr || s->empty())
        return fallback;
    char* end = nullptr;
    const double v = std::strtod(s->c_str(), &end);
    if (end == s->c_str())
        return fallback;
    return static_cast<f32>(v);
}

i32 parseInt(const std::string* s, i32 fallback) {
    if (s == nullptr || s->empty())
        return fallback;
    char* end = nullptr;
    const long v = std::strtol(s->c_str(), &end, 10);
    if (end == s->c_str())
        return fallback;
    return static_cast<i32>(v);
}

bool parseBool(const std::string* s, bool fallback) {
    if (s == nullptr || s->empty())
        return fallback;
    return *s == "true" || *s == "1" || *s == "yes";
}

f32 clamp01(f32 v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

// ── 쓰기 ────────────────────────────────────────────────────────────────

/// stack.xml 을 만들면서 레이어 PNG 를 ZIP 에 밀어 넣는 상태 묶음.
struct Writer {
    ZipWriter& zip;
    const SaveOptions& opts;
    std::string xml;
    int nextLayerIndex = 0;
    LayerId active = kInvalidLayerId;

    void indent(int depth) { xml.append(static_cast<usize>(depth) * 2u, ' '); }

    /// 모든 레이어가 공유하는 속성.
    void commonAttrs(const Layer& layer) {
        xml += " name=\"" + xmlEscape(layer.name()) + "\"";
        xml += " opacity=\"" + formatFloat(clamp01(layer.opacity())) + "\"";
        xml += std::string(" visibility=\"") + (layer.visible() ? "visible" : "hidden") + "\"";
        xml += " composite-op=\"" + std::string(compositeOpName(layer.blendMode())) + "\"";
        // Mari 확장. 네임스페이스를 붙여 두면 표준 리더는 조용히 무시한다.
        if (layer.locked())
            xml += " mari:locked=\"true\"";
        if (layer.alphaLocked())
            xml += " mari:alpha-locked=\"true\"";
        if (layer.id() == active)
            xml += " selected=\"true\"";
    }

    Result<void> writeLayer(const Layer& layer, int depth) {
        const TileMap* tiles = layer.tiles();
        Rect area = tiles != nullptr ? tiles->bounds() : Rect{};
        if (area.isEmpty())
            area = Rect{0, 0, 1, 1}; // 빈 레이어도 src 가 있어야 한다(스펙)

        Image8 img;
        if (tiles != nullptr) {
            auto r = readRegion(*tiles, area);
            if (!r)
                return r.error();
            img = std::move(r).value();
        } else {
            img = Image8::make(area.width, area.height);
        }

        auto png = encodePng(img, opts.compressionLevel);
        if (!png)
            return png.error();

        char nameBuf[64];
        std::snprintf(nameBuf, sizeof(nameBuf), "data/layer%d.png", nextLayerIndex++);
        const std::vector<u8>& bytes = png.value();
        // PNG 는 이미 deflate 로 압축돼 있다. 한 번 더 감싸봐야 CPU 만 쓴다 → Store.
        auto added = zip.add(nameBuf, bytes.data(), bytes.size(), ZipMethod::Store);
        if (!added)
            return added.error();

        indent(depth);
        xml += "<layer";
        commonAttrs(layer);
        xml += " x=\"" + std::to_string(area.x) + "\" y=\"" + std::to_string(area.y) + "\"";
        xml += " src=\"" + std::string(nameBuf) + "\"/>\n";
        return Ok();
    }

    Result<void> writeStack(const std::vector<LayerPtr>& children, int depth) {
        // .ora 는 위에서 아래로 쓴다 — Mari 의 순서를 뒤집는다.
        for (usize i = children.size(); i-- > 0;) {
            const LayerPtr& child = children[i];
            if (!child)
                continue;
            if (child->kind() == LayerKind::Group) {
                indent(depth);
                xml += "<stack";
                commonAttrs(*child);
                xml += " x=\"0\" y=\"0\">\n";
                auto r = writeStack(child->children(), depth + 1);
                if (!r)
                    return r.error();
                indent(depth);
                xml += "</stack>\n";
            } else {
                auto r = writeLayer(*child, depth);
                if (!r)
                    return r.error();
            }
        }
        return Ok();
    }
};

// ── 읽기 ────────────────────────────────────────────────────────────────

struct Reader {
    const ZipReader& zip;
    LayerTree& tree;
    std::vector<std::string>& warnings;

    /// XML 순서(위→아래)를 Mari 순서(아래→위)로 뒤집어 넣는다.
    /// 그룹은 자기 x/y 를 가질 수 있는데 Mari 레이어에는 오프셋 개념이 없으므로
    /// 자식 좌표에 누적해 내려보낸다.
    Result<void> readStack(const XmlNode& stack, LayerId parent, i32 offX, i32 offY) {
        for (usize i = stack.children.size(); i-- > 0;) {
            const XmlNode& node = stack.children[i];
            if (node.name == "stack") {
                auto made = tree.addGroup(node.attrOr("name", "group"), parent, -1);
                if (!made)
                    return made.error();
                applyCommon(*made.value(), node);
                auto r = readStack(node, made.value()->id(), offX + parseInt(node.attr("x"), 0),
                                   offY + parseInt(node.attr("y"), 0));
                if (!r)
                    return r.error();
            } else if (node.name == "layer") {
                auto r = readLayer(node, parent, offX, offY);
                if (!r)
                    return r.error();
            } else {
                warnings.push_back("stack.xml: 모르는 요소를 건너뛴다: " + node.name);
            }
        }
        return Ok();
    }

    void applyCommon(Layer& layer, const XmlNode& node) {
        layer.setOpacity(clamp01(parseFloat(node.attr("opacity"), 1.0f)));
        layer.setVisible(node.attrOr("visibility", "visible") != "hidden");
        layer.setLocked(parseBool(node.attr("mari:locked"), false));
        layer.setAlphaLocked(parseBool(node.attr("mari:alpha-locked"), false));

        const std::string op = node.attrOr("composite-op", "svg:src-over");
        const std::optional<BlendMode> mode = blendModeFromCompositeOp(op);
        if (mode.has_value()) {
            layer.setBlendMode(*mode);
        } else {
            layer.setBlendMode(BlendMode::Normal);
            warnings.push_back("레이어 '" + layer.name() + "': 모르는 composite-op \"" + op +
                               "\" → normal 로 떨어뜨렸다");
        }
        if (parseBool(node.attr("selected"), false))
            (void)tree.setActiveLayer(layer.id());
    }

    Result<void> readLayer(const XmlNode& node, LayerId parent, i32 offX, i32 offY) {
        auto made = tree.addRaster(node.attrOr("name", "layer"), parent, -1);
        if (!made)
            return made.error();
        const LayerPtr layer = made.value();
        applyCommon(*layer, node);

        const std::string src = node.attrOr("src", "");
        if (src.empty()) {
            warnings.push_back("레이어 '" + layer->name() + "': src 가 없어 빈 레이어로 둔다");
            return Ok();
        }
        auto raw = zip.read(src);
        if (!raw) {
            warnings.push_back("레이어 '" + layer->name() + "': " + src +
                               " 를 읽지 못했다 → 빈 레이어로 둔다 (" + raw.message() + ")");
            return Ok();
        }
        const std::vector<u8>& bytes = raw.value();
        auto img = decodePng(bytes.data(), bytes.size());
        if (!img) {
            warnings.push_back("레이어 '" + layer->name() + "': " + src +
                               " PNG 디코드 실패 → 빈 레이어로 둔다 (" + img.message() + ")");
            return Ok();
        }
        const Image8 image = std::move(img).value();
        if (image.empty())
            return Ok();

        TileMap* tiles = layer->tiles();
        if (tiles == nullptr)
            return Err("래스터 레이어에 타일맵이 없다", ErrorCode::Unknown);
        const Rect area{offX + parseInt(node.attr("x"), 0), offY + parseInt(node.attr("y"), 0),
                        image.width, image.height};
        return writeRegion(*tiles, area, image);
    }
};

} // namespace

// ── 파일 유틸 ────────────────────────────────────────────────────────────

Result<std::vector<u8>> readFileBytes(const std::string& path) {
    std::FILE* f = fopenUtf8(path, "rb");
    if (f == nullptr)
        return Err("파일을 열 수 없다: " + path, ErrorCode::IoError);
    if (std::fseek(f, 0, SEEK_END) != 0) {
        std::fclose(f);
        return Err("파일 끝으로 이동 실패: " + path, ErrorCode::IoError);
    }
    const long len = std::ftell(f);
    if (len < 0) {
        std::fclose(f);
        return Err("파일 길이를 알 수 없다: " + path, ErrorCode::IoError);
    }
    std::rewind(f);
    std::vector<u8> bytes(static_cast<usize>(len));
    if (len > 0 && std::fread(bytes.data(), 1, bytes.size(), f) != bytes.size()) {
        std::fclose(f);
        return Err("파일을 다 읽지 못했다: " + path, ErrorCode::IoError);
    }
    std::fclose(f);
    return bytes;
}

Result<void> writeFileBytes(const std::string& path, const u8* data, usize size) {
    std::FILE* f = fopenUtf8(path, "wb");
    if (f == nullptr)
        return Err("파일을 만들 수 없다: " + path, ErrorCode::IoError);
    if (size > 0 && std::fwrite(data, 1, size, f) != size) {
        std::fclose(f);
        return Err("파일을 다 쓰지 못했다: " + path, ErrorCode::IoError);
    }
    if (std::fclose(f) != 0)
        return Err("파일을 닫지 못했다: " + path, ErrorCode::IoError);
    return Ok();
}

// ── 저장 ────────────────────────────────────────────────────────────────

Result<std::vector<u8>> saveToMemory(const LayerTree& tree, const SaveOptions& opts) {
    const Size canvas = tree.canvasSize();
    if (canvas.width < 0 || canvas.height < 0)
        return Err("캔버스 크기가 음수다", ErrorCode::InvalidArgument);

    ZipWriter zip;
    // 스펙: mimetype 은 무압축 첫 항목이어야 한다.
    if (auto r = zip.beginOra(); !r)
        return r.error();

    Writer w{zip, opts, {}, 0, tree.activeLayer()};
    w.xml = "<?xml version='1.0' encoding='UTF-8'?>\n";
    w.xml += "<image version=\"0.0.3\" xmlns:mari=\"https://github.com/mirokim/Mari-paint/ns\"";
    w.xml += " w=\"" + std::to_string(canvas.width) + "\" h=\"" + std::to_string(canvas.height) +
             "\">\n";
    w.xml += "  <stack>\n";
    if (auto r = w.writeStack(tree.roots(), 2); !r)
        return r.error();
    w.xml += "  </stack>\n</image>\n";

    if (auto r = zip.add(kStackEntry, w.xml, ZipMethod::Deflate, opts.compressionLevel); !r)
        return r.error();

    // mergedimage.png / 썸네일. 캔버스가 비었으면 만들 수 없다.
    if ((opts.writeMergedImage || opts.writeThumbnail) && !canvas.isEmpty()) {
        Image8 merged = Image8::make(canvas.width, canvas.height);
        if (merged.pixels.empty())
            return Err("합성 버퍼를 잡지 못했다", ErrorCode::OutOfMemory);
        if (auto r = tree.flatten(Rect{0, 0, canvas.width, canvas.height}, merged.pixels.data(),
                                 merged.stride());
            !r)
            return r.error();

        if (opts.writeMergedImage) {
            auto png = encodePng(merged, opts.compressionLevel);
            if (!png)
                return png.error();
            const std::vector<u8>& b = png.value();
            if (auto r = zip.add(kMergedEntry, b.data(), b.size(), ZipMethod::Store); !r)
                return r.error();
        }
        if (opts.writeThumbnail) {
            const Image8 thumb = downscaleToFit(merged, opts.thumbnailMaxSide > 0
                                                            ? opts.thumbnailMaxSide
                                                            : kThumbnailMaxSide);
            auto png = encodePng(thumb, opts.compressionLevel);
            if (!png)
                return png.error();
            const std::vector<u8>& b = png.value();
            if (auto r = zip.add(kThumbnailEntry, b.data(), b.size(), ZipMethod::Store); !r)
                return r.error();
        }
    }

    // Mari 확장 — 선택 항목이라 표준 리더는 무시한다(docs/03 6절).
    if (opts.proofLog.has_value()) {
        if (auto r = zip.add(kProofLogEntry, *opts.proofLog, ZipMethod::Deflate,
                             opts.compressionLevel);
            !r)
            return r.error();
    }

    return zip.finish();
}

Result<void> save(const LayerTree& tree, const std::string& path, const SaveOptions& opts) {
    auto bytes = saveToMemory(tree, opts);
    if (!bytes)
        return bytes.error();
    const std::vector<u8>& b = bytes.value();
    return writeFileBytes(path, b.data(), b.size());
}

// ── 읽기 ────────────────────────────────────────────────────────────────

Result<Document> loadFromMemory(std::vector<u8> bytes, const LoadOptions& opts) {
    auto opened = ZipReader::open(std::move(bytes));
    if (!opened)
        return opened.error();
    const ZipReader zip = std::move(opened).value();

    // mimetype 검사. 없거나 다르면 .ora 가 아니다.
    auto mime = zip.read(kMimeTypeEntry);
    if (!mime)
        return Err(".ora 가 아니다: mimetype 항목이 없다", ErrorCode::ParseError);
    {
        const std::vector<u8>& m = mime.value();
        const std::string text(reinterpret_cast<const char*>(m.data()), m.size());
        if (text != kMimeType)
            return Err(".ora 가 아니다: mimetype 이 \"" + text + "\"", ErrorCode::ParseError);
    }

    auto stackBytes = zip.read(kStackEntry);
    if (!stackBytes)
        return Err(".ora 가 깨졌다: stack.xml 이 없다", ErrorCode::ParseError);

    const std::vector<u8>& sb = stackBytes.value();
    auto parsed = parseXml(std::string_view(reinterpret_cast<const char*>(sb.data()), sb.size()));
    if (!parsed)
        return parsed.error();
    const XmlNode root = std::move(parsed).value();
    if (root.name != "image")
        return Err("stack.xml 의 루트가 <image> 가 아니다", ErrorCode::ParseError);

    const i32 w = parseInt(root.attr("w"), 0);
    const i32 h = parseInt(root.attr("h"), 0);
    if (w < 0 || h < 0 || w > 1 << 20 || h > 1 << 20)
        return Err("stack.xml 의 캔버스 크기가 말이 안 된다", ErrorCode::ParseError);

    auto made = makeLayerTree(Size{w, h});
    if (!made)
        return made.error();

    Document doc;
    doc.tree = std::move(made).value();

    const XmlNode* topStack = nullptr;
    for (const XmlNode& c : root.children)
        if (c.name == "stack") {
            topStack = &c;
            break;
        }
    if (topStack == nullptr)
        return Err("stack.xml 에 <stack> 이 없다", ErrorCode::ParseError);

    Reader reader{zip, *doc.tree, doc.warnings};
    if (auto r = reader.readStack(*topStack, kInvalidLayerId, 0, 0); !r)
        return r.error();

    if (opts.loadProofLog && zip.contains(kProofLogEntry)) {
        auto log = zip.read(kProofLogEntry);
        if (log) {
            const std::vector<u8>& b = log.value();
            doc.proofLog = std::string(reinterpret_cast<const char*>(b.data()), b.size());
        } else {
            doc.warnings.push_back(std::string(kProofLogEntry) + " 를 읽지 못했다: " +
                                   log.message());
        }
    }
    return doc;
}

Result<Document> load(const std::string& path, const LoadOptions& opts) {
    auto bytes = readFileBytes(path);
    if (!bytes)
        return bytes.error();
    return loadFromMemory(std::move(bytes).value(), opts);
}

Result<std::optional<std::string>> readProofLog(const std::string& path) {
    auto bytes = readFileBytes(path);
    if (!bytes)
        return bytes.error();
    auto opened = ZipReader::open(std::move(bytes).value());
    if (!opened)
        return opened.error();
    const ZipReader zip = std::move(opened).value();
    if (!zip.contains(kProofLogEntry))
        return std::optional<std::string>{};
    auto log = zip.read(kProofLogEntry);
    if (!log)
        return log.error();
    const std::vector<u8>& b = log.value();
    return std::optional<std::string>(
        std::string(reinterpret_cast<const char*>(b.data()), b.size()));
}

} // namespace mari::ora
