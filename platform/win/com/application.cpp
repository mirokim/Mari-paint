// Mari Paint — IMariApplication / IMariDocuments / IMariDocument 구현
//
// 이 파일은 **번역기**다. COM 타입(BSTR·SAFEARRAY·VARIANT_BOOL)과 core 타입
// (std::string·std::vector·bool) 사이를 옮기고, 실제 일은 IApplicationBridge 에
// 넘긴다. 여기에 그림 그리는 로직이 들어오면 의존 방향이 깨진 것이다(docs/02 2절).
#include <mari/win/com/app_bridge.hpp>
#include <mari/win/com/dual_base.hpp>

#include <windows.h>

#include <new>

#include "mari.h"

namespace mari::win::com {
namespace {

IApplicationBridge* g_bridge = nullptr;

/// 브리지가 없으면 COM 객체를 만들지 않는다.
[[nodiscard]] HRESULT requireBridge(IApplicationBridge** out) {
    if (g_bridge == nullptr) {
        *out = nullptr;
        return CO_E_SERVER_STOPPING;
    }
    *out = g_bridge;
    return S_OK;
}

[[nodiscard]] VARIANT_BOOL vb(bool b) noexcept { return b ? VARIANT_TRUE : VARIANT_FALSE; }

/// BSTR → UTF-8. null 은 빈 문자열로 본다(스크립트가 흔히 생략한다).
[[nodiscard]] std::string fromBstr(BSTR b) {
    return b == nullptr ? std::string() : wideToUtf8(std::wstring(b, ::SysStringLen(b)));
}

} // namespace

void setApplicationBridge(IApplicationBridge* bridge) noexcept { g_bridge = bridge; }
IApplicationBridge* applicationBridge() noexcept { return g_bridge; }

// ════════════════════════════════════════════════════════════════════════════
// IMariLayer
// ════════════════════════════════════════════════════════════════════════════

/// 레이어 하나를 감싼다. **레이어를 소유하지 않는다** — 문서가 소유한다.
/// 문서가 닫히면 이 래퍼는 `NotFound` 를 돌려주게 된다(댕글링 대신 정직한 실패).
class MariLayerImpl final : public DualBase<IMariLayer, &IID_IMariLayer> {
public:
    MariLayerImpl(IDocumentBridge* doc, LayerId id) noexcept : doc_(doc), id_(id) {}

    HRESULT STDMETHODCALLTYPE get_Id(LONG* pVal) override {
        return returnScalar<LONG>(pVal, static_cast<LONG>(id_));
    }

    HRESULT STDMETHODCALLTYPE get_Name(BSTR* pVal) override {
        LayerPtr l = layer();
        if (!l) {
            return notFound();
        }
        return returnBstr(pVal, l->name());
    }
    HRESULT STDMETHODCALLTYPE put_Name(BSTR newVal) override {
        LayerPtr l = layer();
        if (!l) {
            return notFound();
        }
        l->setName(fromBstr(newVal));
        fireChanged("name");
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE get_Kind(MariLayerKind* pVal) override {
        LayerPtr l = layer();
        if (!l) {
            return notFound();
        }
        return returnScalar(pVal, static_cast<MariLayerKind>(l->kind()));
    }

    HRESULT STDMETHODCALLTYPE get_Opacity(DOUBLE* pVal) override {
        LayerPtr l = layer();
        if (!l) {
            return notFound();
        }
        return returnScalar<DOUBLE>(pVal, static_cast<DOUBLE>(l->opacity()));
    }
    HRESULT STDMETHODCALLTYPE put_Opacity(DOUBLE newVal) override {
        LayerPtr l = layer();
        if (!l) {
            return notFound();
        }
        if (!(newVal >= 0.0 && newVal <= 1.0)) {
            // NaN 도 여기서 걸린다(비교가 전부 false 다).
            return setErrorInfo(IID_IMariLayer, "불투명도는 0.0~1.0 이어야 한다", E_INVALIDARG);
        }
        l->setOpacity(static_cast<f32>(newVal));
        fireChanged("opacity");
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE get_BlendMode(MariBlendMode* pVal) override {
        LayerPtr l = layer();
        if (!l) {
            return notFound();
        }
        return returnScalar(pVal, static_cast<MariBlendMode>(l->blendMode()));
    }
    HRESULT STDMETHODCALLTYPE put_BlendMode(MariBlendMode newVal) override {
        LayerPtr l = layer();
        if (!l) {
            return notFound();
        }
        const long v = static_cast<long>(newVal);
        if (v < 0 || v > static_cast<long>(BlendMode::Erase)) {
            return setErrorInfo(IID_IMariLayer, "모르는 블렌드 모드다", E_INVALIDARG);
        }
        l->setBlendMode(static_cast<BlendMode>(v));
        fireChanged("blendMode");
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE get_Visible(VARIANT_BOOL* pVal) override {
        LayerPtr l = layer();
        return !l ? notFound() : returnScalar(pVal, vb(l->visible()));
    }
    HRESULT STDMETHODCALLTYPE put_Visible(VARIANT_BOOL newVal) override {
        LayerPtr l = layer();
        if (!l) {
            return notFound();
        }
        l->setVisible(newVal != VARIANT_FALSE);
        fireChanged("visible");
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE get_Locked(VARIANT_BOOL* pVal) override {
        LayerPtr l = layer();
        return !l ? notFound() : returnScalar(pVal, vb(l->locked()));
    }
    HRESULT STDMETHODCALLTYPE put_Locked(VARIANT_BOOL newVal) override {
        LayerPtr l = layer();
        if (!l) {
            return notFound();
        }
        l->setLocked(newVal != VARIANT_FALSE);
        fireChanged("locked");
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE get_AlphaLocked(VARIANT_BOOL* pVal) override {
        LayerPtr l = layer();
        return !l ? notFound() : returnScalar(pVal, vb(l->alphaLocked()));
    }
    HRESULT STDMETHODCALLTYPE put_AlphaLocked(VARIANT_BOOL newVal) override {
        LayerPtr l = layer();
        if (!l) {
            return notFound();
        }
        l->setAlphaLocked(newVal != VARIANT_FALSE);
        fireChanged("alphaLocked");
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE get_BoundsLeft(LONG* p) override { return bounds(p, 0); }
    HRESULT STDMETHODCALLTYPE get_BoundsTop(LONG* p) override { return bounds(p, 1); }
    HRESULT STDMETHODCALLTYPE get_BoundsRight(LONG* p) override { return bounds(p, 2); }
    HRESULT STDMETHODCALLTYPE get_BoundsBottom(LONG* p) override { return bounds(p, 3); }

    HRESULT STDMETHODCALLTYPE get_Children(IMariLayers** ppVal) override;

    HRESULT STDMETHODCALLTYPE ExportPixels(LONG* pWidth, LONG* pHeight,
                                           SAFEARRAY** pData) override;
    HRESULT STDMETHODCALLTYPE ImportPixels(LONG x, LONG y, LONG width, LONG height,
                                           SAFEARRAY* data) override;

    HRESULT STDMETHODCALLTYPE PixelHash(BSTR* pVal) override {
        if (doc_ == nullptr) {
            return notFound();
        }
        auto r = doc_->layerHash(id_);
        if (!r.ok()) {
            return hresultFromError(IID_IMariLayer, r.error());
        }
        return returnBstr(pVal, r.value());
    }

private:
    /// 🔴 core 의 find() 는 shared_ptr 을 돌려준다. **매 호출마다 새로 받는다** —
    ///    포인터를 캐시하면 레이어가 지워진 뒤 댕글링한다.
    [[nodiscard]] LayerPtr layer() {
        if (doc_ == nullptr) {
            return nullptr;
        }
        return doc_->layers().find(id_);
    }
    [[nodiscard]] static HRESULT notFound() {
        return setErrorInfo(IID_IMariLayer,
                            "이 레이어는 더 이상 없다(문서가 닫혔거나 레이어가 지워졌다)",
                            HRESULT_FROM_WIN32(ERROR_NOT_FOUND));
    }
    [[nodiscard]] HRESULT bounds(LONG* p, int which) {
        LayerPtr l = layer();
        if (!l) {
            return notFound();
        }
        const Rect b = l->bounds();
        const i32 v = which == 0 ? b.x : which == 1 ? b.y : which == 2 ? b.right() : b.bottom();
        return returnScalar<LONG>(p, static_cast<LONG>(v));
    }
    void fireChanged(const char* what) {
        if (g_bridge != nullptr) {
            g_bridge->events().fireLayerChanged(static_cast<i32>(id_), what);
        }
    }

    IDocumentBridge* doc_;
    LayerId id_;
};

// ════════════════════════════════════════════════════════════════════════════
// IMariLayers — 🔴 인덱스 0 = 가장 아래 (core 규약. 포토샵 COM 과 반대다)
// ════════════════════════════════════════════════════════════════════════════

class MariLayersImpl final : public DualBase<IMariLayers, &IID_IMariLayers> {
public:
    /// parent == kInvalidLayerId 이면 루트 목록이다.
    MariLayersImpl(IDocumentBridge* doc, LayerId parent) noexcept : doc_(doc), parent_(parent) {}

    HRESULT STDMETHODCALLTYPE get_Item(LONG index, IMariLayer** ppVal) override {
        if (ppVal == nullptr) {
            return E_POINTER;
        }
        *ppVal = nullptr;
        const std::vector<LayerId> ids = list();
        if (index < 0 || static_cast<usize>(index) >= ids.size()) {
            return setErrorInfo(IID_IMariLayers, "레이어 인덱스가 범위를 벗어났다", DISP_E_BADINDEX);
        }
        *ppVal = new (std::nothrow) MariLayerImpl(doc_, ids[static_cast<usize>(index)]);
        return *ppVal != nullptr ? S_OK : E_OUTOFMEMORY;
    }

    HRESULT STDMETHODCALLTYPE get_Count(LONG* pVal) override {
        return returnScalar<LONG>(pVal, static_cast<LONG>(list().size()));
    }

    HRESULT STDMETHODCALLTYPE get__NewEnum(IUnknown** ppVal) override {
        // ⚠️ for-each(IEnumVARIANT) 는 **아직 구현하지 않았다.**
        //    조용히 빈 열거자를 주면 스크립트가 "레이어가 0개"라고 착각한다 —
        //    그게 조용히 틀린 답을 주는 최악의 형태다. 정직하게 미구현으로 답한다.
        //    Count/Item 으로 도는 코드는 전부 정상 동작한다:
        //        for i in range(doc.Layers.Count): doc.Layers(i)
        if (ppVal != nullptr) {
            *ppVal = nullptr;
        }
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE AddRaster(BSTR name, LONG index, IMariLayer** ppVal) override {
        return add(name, index, false, ppVal);
    }
    HRESULT STDMETHODCALLTYPE AddGroup(BSTR name, LONG index, IMariLayer** ppVal) override {
        return add(name, index, true, ppVal);
    }

    HRESULT STDMETHODCALLTYPE Remove(LONG id) override {
        if (doc_ == nullptr) {
            return CO_E_OBJNOTCONNECTED;
        }
        auto r = doc_->layers().remove(static_cast<LayerId>(id));
        if (!r.ok()) {
            return hresultFromError(IID_IMariLayers, r.error());
        }
        if (g_bridge != nullptr) {
            g_bridge->events().fireLayerChanged(id, "removed");
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Move(LONG id, LONG parentId, LONG index) override {
        if (doc_ == nullptr) {
            return CO_E_OBJNOTCONNECTED;
        }
        auto r = doc_->layers().move(static_cast<LayerId>(id), static_cast<LayerId>(parentId),
                                     static_cast<i32>(index));
        if (!r.ok()) {
            return hresultFromError(IID_IMariLayers, r.error());
        }
        if (g_bridge != nullptr) {
            g_bridge->events().fireLayerChanged(id, "moved");
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE FindById(LONG id, IMariLayer** ppVal) override {
        if (ppVal == nullptr) {
            return E_POINTER;
        }
        *ppVal = nullptr;
        if (doc_ == nullptr || !doc_->layers().find(static_cast<LayerId>(id))) {
            return setErrorInfo(IID_IMariLayers, "그런 레이어가 없다",
                                HRESULT_FROM_WIN32(ERROR_NOT_FOUND));
        }
        *ppVal = new (std::nothrow) MariLayerImpl(doc_, static_cast<LayerId>(id));
        return *ppVal != nullptr ? S_OK : E_OUTOFMEMORY;
    }

private:
    [[nodiscard]] std::vector<LayerId> list() const {
        std::vector<LayerId> ids;
        if (doc_ == nullptr) {
            return ids;
        }
        LayerTree& tree = doc_->layers();
        if (parent_ == kInvalidLayerId) {
            for (const LayerPtr& l : tree.roots()) {
                ids.push_back(l->id());
            }
        } else if (LayerPtr p = tree.find(parent_); p) {
            for (const LayerPtr& l : p->children()) {
                ids.push_back(l->id());
            }
        }
        return ids;
    }

    HRESULT add(BSTR name, LONG index, bool group, IMariLayer** ppVal) {
        if (ppVal == nullptr) {
            return E_POINTER;
        }
        *ppVal = nullptr;
        if (doc_ == nullptr) {
            return CO_E_OBJNOTCONNECTED;
        }
        const std::string n = fromBstr(name);
        LayerTree& tree = doc_->layers();
        auto r = group ? tree.addGroup(n, parent_, static_cast<i32>(index))
                       : tree.addRaster(n, parent_, static_cast<i32>(index));
        if (!r.ok()) {
            return hresultFromError(IID_IMariLayers, r.error());
        }
        const LayerId id = r.value()->id();
        if (g_bridge != nullptr) {
            g_bridge->events().fireLayerChanged(static_cast<i32>(id), group ? "addedGroup"
                                                                            : "addedRaster");
        }
        *ppVal = new (std::nothrow) MariLayerImpl(doc_, id);
        return *ppVal != nullptr ? S_OK : E_OUTOFMEMORY;
    }

    IDocumentBridge* doc_;
    LayerId parent_;
};

HRESULT MariLayerImpl::get_Children(IMariLayers** ppVal) {
    if (ppVal == nullptr) {
        return E_POINTER;
    }
    *ppVal = new (std::nothrow) MariLayersImpl(doc_, id_);
    return *ppVal != nullptr ? S_OK : E_OUTOFMEMORY;
}

HRESULT MariLayerImpl::ExportPixels(LONG* pWidth, LONG* pHeight, SAFEARRAY** pData) {
    if (pWidth == nullptr || pHeight == nullptr || pData == nullptr) {
        return E_POINTER;
    }
    *pWidth = 0;
    *pHeight = 0;
    *pData = nullptr;
    if (doc_ == nullptr) {
        return notFound();
    }

    // 🔴 타일을 직접 훑지 않는다. 그건 core/compositor 의 일이다.
    //    여기서 손으로 블리팅하면 블렌드·마스크·알파 규칙이 두 군데로 갈라지고,
    //    언젠가 한쪽만 고쳐진다. 이 파일은 **번역기**지 합성기가 아니다.
    std::vector<u8> buf;
    Rect area{};
    auto r = doc_->exportLayerPixels(id_, buf, area);
    if (!r.ok()) {
        return hresultFromError(IID_IMariLayer, r.error());
    }
    if (area.width <= 0 || area.height <= 0) {
        // 빈 레이어. 0×0 빈 배열을 정직하게 돌려준다(에러가 아니다).
        *pData = safeArrayFromBytes(nullptr, 0);
        return *pData != nullptr ? S_OK : E_OUTOFMEMORY;
    }

    // ⚠️ 여기는 편의 경로다. 큰 레이어를 SAFEARRAY 마샬링으로 넘기지 마라 —
    //    8bf 처럼 공유 메모리를 써야 한다(docs/02 6.2).
    if (buf.size() > (256u << 20)) {
        return setErrorInfo(IID_IMariLayer,
                            "레이어가 너무 커서 COM 으로 내보낼 수 없다(256MB 초과). "
                            "영역을 나눠서 가져가라",
                            E_INVALIDARG);
    }

    *pWidth = static_cast<LONG>(area.width);
    *pHeight = static_cast<LONG>(area.height);
    *pData = safeArrayFromBytes(buf.data(), buf.size());
    return *pData != nullptr ? S_OK : E_OUTOFMEMORY;
}

HRESULT MariLayerImpl::ImportPixels(LONG x, LONG y, LONG width, LONG height, SAFEARRAY* data) {
    if (doc_ == nullptr) {
        return notFound();
    }
    if (width <= 0 || height <= 0) {
        return setErrorInfo(IID_IMariLayer, "가로·세로는 0보다 커야 한다", E_INVALIDARG);
    }
    const unsigned char* bytes = nullptr;
    size_t len = 0;
    const HRESULT hr = bytesFromSafeArray(data, &bytes, &len);
    if (FAILED(hr)) {
        return setErrorInfo(IID_IMariLayer, "픽셀 배열은 1차원 BYTE 배열이어야 한다", hr);
    }
    // 🔴 크기를 반드시 확인한다. 스크립트가 짧은 배열을 넘기면 버퍼 밖을 읽는다.
    const u64 need = static_cast<u64>(width) * static_cast<u64>(height) * 4ull;
    if (static_cast<u64>(len) < need) {
        unlockSafeArray(data);
        return setErrorInfo(IID_IMariLayer,
                            "픽셀 배열이 width*height*4 보다 작다", E_INVALIDARG);
    }
    auto r = doc_->importPixels(std::string(), bytes, len, static_cast<i32>(width),
                                static_cast<i32>(height));
    unlockSafeArray(data);
    if (!r.ok()) {
        return hresultFromError(IID_IMariLayer, r.error());
    }
    (void)x;
    (void)y;
    // 🔴 붓으로 그리지 않은 픽셀이다. 정직하게 보고한다.
    if (g_bridge != nullptr) {
        g_bridge->events().firePaste(PasteSource::Script);
    }
    return S_OK;
}

// ════════════════════════════════════════════════════════════════════════════
// IMariSelection
// ════════════════════════════════════════════════════════════════════════════

class MariSelectionImpl final : public DualBase<IMariSelection, &IID_IMariSelection> {
public:
    explicit MariSelectionImpl(IDocumentBridge* doc) noexcept : doc_(doc) {}

    HRESULT STDMETHODCALLTYPE get_IsEmpty(VARIANT_BOOL* p) override {
        return returnScalar(p, vb(doc_ == nullptr || doc_->selection().isEmpty()));
    }
    HRESULT STDMETHODCALLTYPE get_Left(LONG* p) override { return part(p, 0); }
    HRESULT STDMETHODCALLTYPE get_Top(LONG* p) override { return part(p, 1); }
    HRESULT STDMETHODCALLTYPE get_Right(LONG* p) override { return part(p, 2); }
    HRESULT STDMETHODCALLTYPE get_Bottom(LONG* p) override { return part(p, 3); }

    HRESULT STDMETHODCALLTYPE SelectRect(LONG x, LONG y, LONG w, LONG h) override {
        if (doc_ == nullptr) {
            return CO_E_OBJNOTCONNECTED;
        }
        if (w < 0 || h < 0) {
            return setErrorInfo(IID_IMariSelection, "선택 영역 크기는 음수일 수 없다",
                                E_INVALIDARG);
        }
        doc_->setSelection(Rect{static_cast<i32>(x), static_cast<i32>(y), static_cast<i32>(w),
                                static_cast<i32>(h)});
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SelectAll() override {
        if (doc_ == nullptr) {
            return CO_E_OBJNOTCONNECTED;
        }
        const Size s = doc_->canvasSize();
        doc_->setSelection(Rect{0, 0, s.width, s.height});
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Deselect() override {
        if (doc_ == nullptr) {
            return CO_E_OBJNOTCONNECTED;
        }
        doc_->setSelection(Rect{});
        return S_OK;
    }

private:
    [[nodiscard]] HRESULT part(LONG* p, int which) {
        if (doc_ == nullptr) {
            return CO_E_OBJNOTCONNECTED;
        }
        const Rect r = doc_->selection();
        const i32 v = which == 0 ? r.x : which == 1 ? r.y : which == 2 ? r.right() : r.bottom();
        return returnScalar<LONG>(p, static_cast<LONG>(v));
    }
    IDocumentBridge* doc_;
};

// ════════════════════════════════════════════════════════════════════════════
// IMariDocument
// ════════════════════════════════════════════════════════════════════════════

class MariDocumentImpl final : public DualBase<IMariDocument, &IID_IMariDocument> {
public:
    explicit MariDocumentImpl(IDocumentBridge* doc) noexcept : doc_(doc) {}

    /// 🔴 Close() 뒤에는 doc_ 이 nullptr 이다. 스크립트가 닫힌 문서를 계속 쥐고
    ///    있는 건 흔한 일이다 — **크래시 대신 오류를 돌려준다.**
    ///    이 검사를 한 군데라도 빼먹으면 Python 한 줄이 Mari 를 죽인다.
#define MARI_REQUIRE_DOC()                                                                         \
    do {                                                                                           \
        if (doc_ == nullptr) {                                                                     \
            return setErrorInfo(IID_IMariDocument, "이 문서는 이미 닫혔다",                        \
                                CO_E_OBJNOTCONNECTED);                                             \
        }                                                                                          \
    } while (false)

    HRESULT STDMETHODCALLTYPE get_Layers(IMariLayers** ppVal) override {
        MARI_REQUIRE_DOC();
        if (ppVal == nullptr) {
            return E_POINTER;
        }
        *ppVal = new (std::nothrow) MariLayersImpl(doc_, kInvalidLayerId);
        return *ppVal != nullptr ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE get_Width(LONG* pVal) override {
        MARI_REQUIRE_DOC();
        return returnScalar<LONG>(pVal, static_cast<LONG>(doc_->canvasSize().width));
    }
    HRESULT STDMETHODCALLTYPE get_Height(LONG* pVal) override {
        MARI_REQUIRE_DOC();
        return returnScalar<LONG>(pVal, static_cast<LONG>(doc_->canvasSize().height));
    }
    HRESULT STDMETHODCALLTYPE get_FullName(BSTR* pVal) override {
        MARI_REQUIRE_DOC();
        return returnBstr(pVal, doc_->fullPath());
    }
    HRESULT STDMETHODCALLTYPE get_Saved(VARIANT_BOOL* pVal) override {
        MARI_REQUIRE_DOC();
        return returnScalar(pVal, vb(doc_->isSaved()));
    }
    HRESULT STDMETHODCALLTYPE get_Selection(IMariSelection** ppVal) override {
        MARI_REQUIRE_DOC();
        if (ppVal == nullptr) {
            return E_POINTER;
        }
        *ppVal = new (std::nothrow) MariSelectionImpl(doc_);
        return *ppVal != nullptr ? S_OK : E_OUTOFMEMORY;
    }

    HRESULT STDMETHODCALLTYPE get_ActiveLayer(IMariLayer** ppVal) override {
        MARI_REQUIRE_DOC();
        if (ppVal == nullptr) {
            return E_POINTER;
        }
        *ppVal = nullptr;
        // 🔴 core 의 activeLayer() 는 Layer* 가 아니라 LayerId 를 돌려준다.
        const LayerId id = doc_->layers().activeLayer();
        if (id == kInvalidLayerId) {
            return S_FALSE; // 활성 레이어가 없는 건 오류가 아니다
        }
        *ppVal = new (std::nothrow) MariLayerImpl(doc_, id);
        return *ppVal != nullptr ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE putref_ActiveLayer(IMariLayer* newVal) override {
        MARI_REQUIRE_DOC();
        if (newVal == nullptr) {
            return E_POINTER;
        }
        LONG id = 0;
        const HRESULT hr = newVal->get_Id(&id);
        if (FAILED(hr)) {
            return hr;
        }
        auto r = doc_->layers().setActiveLayer(static_cast<LayerId>(id));
        return r.ok() ? S_OK : hresultFromError(IID_IMariDocument, r.error());
    }

    // ── 뷰 (docs/03 7절 두 증인 대조) ───────────────────────────────────
    HRESULT STDMETHODCALLTYPE get_ViewZoom(DOUBLE* pVal) override {
        MARI_REQUIRE_DOC();
        return returnScalar<DOUBLE>(pVal, doc_->viewState().zoom);
    }
    HRESULT STDMETHODCALLTYPE get_ViewRotation(DOUBLE* pVal) override {
        MARI_REQUIRE_DOC();
        return returnScalar<DOUBLE>(pVal, doc_->viewState().rotationDeg);
    }
    HRESULT STDMETHODCALLTYPE GetViewMatrix(SAFEARRAY** pVal) override {
        MARI_REQUIRE_DOC();
        if (pVal == nullptr) {
            return E_POINTER;
        }
        *pVal = nullptr;
        const ViewTransform v(doc_->viewState());
        const ViewReport r = makeViewReport(v);
        const double m[6] = {r.a, r.b, r.tx, r.c, r.d, r.ty};
        SAFEARRAY* sa = ::SafeArrayCreateVector(VT_R8, 0, 6);
        if (sa == nullptr) {
            return E_OUTOFMEMORY;
        }
        void* p = nullptr;
        if (FAILED(::SafeArrayAccessData(sa, &p))) {
            ::SafeArrayDestroy(sa);
            return E_FAIL;
        }
        ::memcpy(p, m, sizeof(m));
        ::SafeArrayUnaccessData(sa);
        *pVal = sa;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Save() override {
        MARI_REQUIRE_DOC();
        auto r = doc_->save();
        return r.ok() ? S_OK : hresultFromError(IID_IMariDocument, r.error());
        // 🔴 OnDocumentSaved 는 본체가 실제로 파일을 쓰고 해시를 낸 뒤 발행한다.
        //    여기서 미리 쏘면 "저장했다고 했는데 파일이 없는" 증거가 남는다.
    }
    HRESULT STDMETHODCALLTYPE SaveAs(BSTR path, BSTR format) override {
        MARI_REQUIRE_DOC();
        const std::string p = fromBstr(path);
        if (p.empty()) {
            return setErrorInfo(IID_IMariDocument, "저장 경로가 비었다", E_INVALIDARG);
        }
        std::string f = fromBstr(format);
        if (f.empty()) {
            f = "ora"; // 네이티브 포맷
        }
        auto r = doc_->saveAs(p, f);
        return r.ok() ? S_OK : hresultFromError(IID_IMariDocument, r.error());
    }
    HRESULT STDMETHODCALLTYPE Close(VARIANT_BOOL saveChanges) override {
        MARI_REQUIRE_DOC();
        auto r = doc_->close(saveChanges != VARIANT_FALSE);
        if (!r.ok()) {
            return hresultFromError(IID_IMariDocument, r.error());
        }
        doc_ = nullptr; // 이 래퍼는 이제 죽은 문서를 가리킨다
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE ExportPixels(LONG* pWidth, LONG* pHeight,
                                           SAFEARRAY** pData) override {
        MARI_REQUIRE_DOC();
        if (pWidth == nullptr || pHeight == nullptr || pData == nullptr) {
            return E_POINTER;
        }
        *pData = nullptr;
        std::vector<u8> buf;
        auto r = doc_->exportComposite(buf);
        if (!r.ok()) {
            return hresultFromError(IID_IMariDocument, r.error());
        }
        const Size s = doc_->canvasSize();
        *pWidth = static_cast<LONG>(s.width);
        *pHeight = static_cast<LONG>(s.height);
        *pData = safeArrayFromBytes(buf.data(), buf.size());
        return *pData != nullptr ? S_OK : E_OUTOFMEMORY;
    }

    HRESULT STDMETHODCALLTYPE ImportPixels(BSTR layerName, SAFEARRAY* data, LONG width,
                                           LONG height, IMariLayer** ppVal) override {
        MARI_REQUIRE_DOC();
        if (ppVal == nullptr) {
            return E_POINTER;
        }
        *ppVal = nullptr;
        if (width <= 0 || height <= 0) {
            return setErrorInfo(IID_IMariDocument, "가로·세로는 0보다 커야 한다", E_INVALIDARG);
        }
        const unsigned char* bytes = nullptr;
        size_t len = 0;
        HRESULT hr = bytesFromSafeArray(data, &bytes, &len);
        if (FAILED(hr)) {
            return setErrorInfo(IID_IMariDocument, "픽셀 배열은 1차원 BYTE 배열이어야 한다", hr);
        }
        const u64 need = static_cast<u64>(width) * static_cast<u64>(height) * 4ull;
        if (static_cast<u64>(len) < need) {
            unlockSafeArray(data);
            return setErrorInfo(IID_IMariDocument, "픽셀 배열이 width*height*4 보다 작다",
                                E_INVALIDARG);
        }
        auto r = doc_->importPixels(fromBstr(layerName), bytes, len, static_cast<i32>(width),
                                    static_cast<i32>(height));
        unlockSafeArray(data);
        if (!r.ok()) {
            return hresultFromError(IID_IMariDocument, r.error());
        }
        // 🔴 스크립트가 넣은 픽셀이다. 숨기지 않는다(docs/03 2절 정신).
        if (g_bridge != nullptr) {
            g_bridge->events().firePaste(PasteSource::Script);
        }
        *ppVal = new (std::nothrow) MariLayerImpl(doc_, r.value());
        return *ppVal != nullptr ? S_OK : E_OUTOFMEMORY;
    }

    HRESULT STDMETHODCALLTYPE CanvasHash(BSTR* pVal) override {
        MARI_REQUIRE_DOC();
        auto r = doc_->canvasHash();
        if (!r.ok()) {
            return hresultFromError(IID_IMariDocument, r.error());
        }
        return returnBstr(pVal, r.value());
    }

    HRESULT STDMETHODCALLTYPE TakeCanvasSnapshot(BSTR* pHash) override {
        MARI_REQUIRE_DOC();
        auto r = doc_->canvasHash();
        if (!r.ok()) {
            return hresultFromError(IID_IMariDocument, r.error());
        }
        // 🔴 docs/03 S3 — StartCanvasHash/EndCanvasHash 가 v0 null 을 벗어나는 지점.
        if (g_bridge != nullptr) {
            g_bridge->events().fireCanvasSnapshot(r.value());
        }
        return returnBstr(pHash, r.value());
    }

    HRESULT STDMETHODCALLTYPE Undo() override {
        MARI_REQUIRE_DOC();
        auto r = doc_->undo();
        if (!r.ok()) {
            return hresultFromError(IID_IMariDocument, r.error());
        }
        if (g_bridge != nullptr) {
            g_bridge->events().fireUndo(1);
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Redo() override {
        MARI_REQUIRE_DOC();
        auto r = doc_->redo();
        if (!r.ok()) {
            return hresultFromError(IID_IMariDocument, r.error());
        }
        if (g_bridge != nullptr) {
            g_bridge->events().fireUndo(-1);
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Apply8bf(BSTR pluginPath, BSTR filterName, LONG timeoutMs,
                                       MariHostStatus* pStatus) override {
        MARI_REQUIRE_DOC();
        if (pStatus == nullptr) {
            return E_POINTER;
        }
        *pStatus = mariHostLoadFailed;
        std::vector<std::string> notes;
        auto r = doc_->apply8bf(fromBstr(pluginPath), fromBstr(filterName),
                                static_cast<i32>(timeoutMs), notes);
        if (!r.ok()) {
            return hresultFromError(IID_IMariDocument, r.error());
        }
        *pStatus = static_cast<MariHostStatus>(r.value());
        // 🔴 호스트가 죽어도(=PluginCrashed) 본체는 산다. 여기서 예외를 던지지 않는다 —
        //    상태 코드로 알린다. docs/02 6.2 "호스트가 죽으면? → 에러만 표시, 본체는 멀쩡"
        return S_OK;
    }

private:
    IDocumentBridge* doc_;
};

#undef MARI_REQUIRE_DOC

// ════════════════════════════════════════════════════════════════════════════
// IMariDocuments
// ════════════════════════════════════════════════════════════════════════════

class MariDocumentsImpl final : public DualBase<IMariDocuments, &IID_IMariDocuments> {
public:
    HRESULT STDMETHODCALLTYPE get_Item(LONG index, IMariDocument** ppVal) override {
        if (ppVal == nullptr) {
            return E_POINTER;
        }
        *ppVal = nullptr;
        IApplicationBridge* app = nullptr;
        const HRESULT hr = requireBridge(&app);
        if (FAILED(hr)) {
            return hr;
        }
        if (index < 0) {
            return DISP_E_BADINDEX;
        }
        IDocumentBridge* d = app->documentAt(static_cast<usize>(index));
        if (d == nullptr) {
            return setErrorInfo(IID_IMariDocuments, "문서 인덱스가 범위를 벗어났다",
                                DISP_E_BADINDEX);
        }
        *ppVal = new (std::nothrow) MariDocumentImpl(d);
        return *ppVal != nullptr ? S_OK : E_OUTOFMEMORY;
    }

    HRESULT STDMETHODCALLTYPE get_Count(LONG* pVal) override {
        IApplicationBridge* app = nullptr;
        const HRESULT hr = requireBridge(&app);
        if (FAILED(hr)) {
            return hr;
        }
        return returnScalar<LONG>(pVal, static_cast<LONG>(app->documentCount()));
    }

    HRESULT STDMETHODCALLTYPE get__NewEnum(IUnknown** ppVal) override {
        if (ppVal != nullptr) {
            *ppVal = nullptr;
        }
        return E_NOTIMPL; // Count/Item 으로 돌아라
    }

    HRESULT STDMETHODCALLTYPE Add(LONG width, LONG height, IMariDocument** ppVal) override {
        if (ppVal == nullptr) {
            return E_POINTER;
        }
        *ppVal = nullptr;
        IApplicationBridge* app = nullptr;
        const HRESULT hr = requireBridge(&app);
        if (FAILED(hr)) {
            return hr;
        }
        auto r = app->createDocument(static_cast<i32>(width), static_cast<i32>(height));
        if (!r.ok()) {
            return hresultFromError(IID_IMariDocuments, r.error());
        }
        *ppVal = new (std::nothrow) MariDocumentImpl(r.value());
        return *ppVal != nullptr ? S_OK : E_OUTOFMEMORY;
    }

    HRESULT STDMETHODCALLTYPE Open(BSTR path, IMariDocument** ppVal) override {
        if (ppVal == nullptr) {
            return E_POINTER;
        }
        *ppVal = nullptr;
        IApplicationBridge* app = nullptr;
        const HRESULT hr = requireBridge(&app);
        if (FAILED(hr)) {
            return hr;
        }
        auto r = app->open(fromBstr(path));
        if (!r.ok()) {
            return hresultFromError(IID_IMariDocuments, r.error());
        }
        *ppVal = new (std::nothrow) MariDocumentImpl(r.value());
        return *ppVal != nullptr ? S_OK : E_OUTOFMEMORY;
    }
};

// ════════════════════════════════════════════════════════════════════════════
// IMariApplication
// ════════════════════════════════════════════════════════════════════════════

class MariApplicationImpl final : public DualBase<IMariApplication, &IID_IMariApplication>,
                                  public ConnectionPointContainer {
    /// 🔴 IUnknown 메서드가 두 베이스에 다 있다. 이름을 못 박아 모호성을 없앤다.
    ///    (ConnectionPointContainer 쪽은 owner() 로 이쪽에 되돌아온다.)
    using Base = DualBase<IMariApplication, &IID_IMariApplication>;

public:
    MariApplicationImpl() noexcept
        : ConnectionPointContainer(g_bridge != nullptr ? &g_bridge->events() : nullptr) {}

    HRESULT STDMETHODCALLTYPE get_Documents(IMariDocuments** ppVal) override {
        if (ppVal == nullptr) {
            return E_POINTER;
        }
        *ppVal = new (std::nothrow) MariDocumentsImpl();
        return *ppVal != nullptr ? S_OK : E_OUTOFMEMORY;
    }

    HRESULT STDMETHODCALLTYPE get_ActiveDocument(IMariDocument** ppVal) override {
        if (ppVal == nullptr) {
            return E_POINTER;
        }
        *ppVal = nullptr;
        IApplicationBridge* app = nullptr;
        const HRESULT hr = requireBridge(&app);
        if (FAILED(hr)) {
            return hr;
        }
        IDocumentBridge* d = app->activeDocument();
        if (d == nullptr) {
            return S_FALSE; // 열린 문서가 없는 건 오류가 아니다
        }
        *ppVal = new (std::nothrow) MariDocumentImpl(d);
        return *ppVal != nullptr ? S_OK : E_OUTOFMEMORY;
    }

    HRESULT STDMETHODCALLTYPE get_Version(BSTR* pVal) override {
        IApplicationBridge* app = nullptr;
        const HRESULT hr = requireBridge(&app);
        if (FAILED(hr)) {
            return hr;
        }
        return returnBstr(pVal, app->version());
    }

    HRESULT STDMETHODCALLTYPE get_Visible(VARIANT_BOOL* pVal) override {
        IApplicationBridge* app = nullptr;
        const HRESULT hr = requireBridge(&app);
        if (FAILED(hr)) {
            return hr;
        }
        return returnScalar(pVal, vb(app->visible()));
    }
    HRESULT STDMETHODCALLTYPE put_Visible(VARIANT_BOOL newVal) override {
        IApplicationBridge* app = nullptr;
        const HRESULT hr = requireBridge(&app);
        if (FAILED(hr)) {
            return hr;
        }
        app->setVisible(newVal != VARIANT_FALSE);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Open(BSTR path, IMariDocument** ppVal) override {
        // ⚠️ COM 객체를 스택에 만들면 안 된다 — Release() 가 `delete this` 를 부른다.
        //    편의 메서드는 브리지를 직접 쓴다.
        if (ppVal == nullptr) {
            return E_POINTER;
        }
        *ppVal = nullptr;
        IApplicationBridge* app = nullptr;
        const HRESULT hr = requireBridge(&app);
        if (FAILED(hr)) {
            return hr;
        }
        auto r = app->open(fromBstr(path));
        if (!r.ok()) {
            return hresultFromError(IID_IMariApplication, r.error());
        }
        *ppVal = new (std::nothrow) MariDocumentImpl(r.value());
        return *ppVal != nullptr ? S_OK : E_OUTOFMEMORY;
    }

    HRESULT STDMETHODCALLTYPE CreateDocument(LONG width, LONG height,
                                             IMariDocument** ppVal) override {
        if (ppVal == nullptr) {
            return E_POINTER;
        }
        *ppVal = nullptr;
        IApplicationBridge* app = nullptr;
        const HRESULT hr = requireBridge(&app);
        if (FAILED(hr)) {
            return hr;
        }
        auto r = app->createDocument(static_cast<i32>(width), static_cast<i32>(height));
        if (!r.ok()) {
            return hresultFromError(IID_IMariApplication, r.error());
        }
        *ppVal = new (std::nothrow) MariDocumentImpl(r.value());
        return *ppVal != nullptr ? S_OK : E_OUTOFMEMORY;
    }

    HRESULT STDMETHODCALLTYPE Quit() override {
        IApplicationBridge* app = nullptr;
        const HRESULT hr = requireBridge(&app);
        if (FAILED(hr)) {
            return hr;
        }
        app->quit();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE AdviseEvents(IMariEventSink* sink, LONG* pCookie) override {
        if (pCookie == nullptr) {
            return E_POINTER;
        }
        *pCookie = 0;
        IApplicationBridge* app = nullptr;
        const HRESULT hr = requireBridge(&app);
        if (FAILED(hr)) {
            return hr;
        }
        if (sink == nullptr) {
            return E_POINTER;
        }
        const long cookie = app->events().advise(sink);
        if (cookie == 0) {
            return setErrorInfo(IID_IMariApplication, "이벤트 싱크를 걸지 못했다",
                                CONNECT_E_CANNOTCONNECT);
        }
        *pCookie = cookie;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE UnadviseEvents(LONG cookie) override {
        IApplicationBridge* app = nullptr;
        const HRESULT hr = requireBridge(&app);
        if (FAILED(hr)) {
            return hr;
        }
        return app->events().unadvise(cookie) ? S_OK : CONNECT_E_NOCONNECTION;
    }

    // ── Sigan 상태 (docs/03 5.1 — 토글 없음, 표시만) ─────────────────────
    HRESULT STDMETHODCALLTYPE get_SiganConnected(VARIANT_BOOL* pVal) override {
        IApplicationBridge* app = nullptr;
        const HRESULT hr = requireBridge(&app);
        if (FAILED(hr)) {
            return hr;
        }
        return returnScalar(pVal, vb(app->siganConnected()));
    }
    HRESULT STDMETHODCALLTYPE get_SiganSpooledFrames(LONGLONG* pVal) override {
        IApplicationBridge* app = nullptr;
        const HRESULT hr = requireBridge(&app);
        if (FAILED(hr)) {
            return hr;
        }
        return returnScalar<LONGLONG>(pVal, static_cast<LONGLONG>(app->siganSpooledFrames()));
    }
    HRESULT STDMETHODCALLTYPE get_SiganLastSeq(LONGLONG* pVal) override {
        IApplicationBridge* app = nullptr;
        const HRESULT hr = requireBridge(&app);
        if (FAILED(hr)) {
            return hr;
        }
        return returnScalar<LONGLONG>(pVal, static_cast<LONGLONG>(app->siganLastSeq()));
    }

    HRESULT STDMETHODCALLTYPE get_PenInputApi(BSTR* pVal) override {
        // 🔴 상수다. 조회해서 만들지 않는다. WinTab 은 구현하지 않는다(docs/03 3절).
        return returnBstr(pVal, kPenInputApi);
    }

protected:
    /// 연결점 컨테이너도 같이 내놓는다(C# 의 event, VB6 의 WithEvents 용).
    HRESULT queryExtra(REFIID riid, void** ppv) override {
        if (riid == IID_IConnectionPointContainer) {
            *ppv = static_cast<IConnectionPointContainer*>(this);
            Base::AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    IUnknown* owner() noexcept override {
        // DualBase 쪽 IUnknown 이 정본이다. COM 규칙상 객체 하나에 IUnknown 은 하나다.
        return static_cast<IUnknown*>(static_cast<IMariApplication*>(this));
    }
};

/// 클래스 팩토리가 부른다.
HRESULT createMariApplication(REFIID riid, void** ppv) {
    auto* obj = new (std::nothrow) MariApplicationImpl();
    if (obj == nullptr) {
        return E_OUTOFMEMORY;
    }
    // DualBase 와 ConnectionPointContainer 양쪽에 IUnknown 이 있어 모호하다.
    // 정본은 owner() 가 말하는 대로 DualBase 쪽이다.
    IUnknown* unk = static_cast<IMariApplication*>(obj);
    const HRESULT hr = unk->QueryInterface(riid, ppv);
    unk->Release();
    return hr;
}

} // namespace mari::win::com
