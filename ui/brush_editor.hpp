// Mari Paint — 브러시 편집기: 팁 · 동작 · 동적 반응 · 텍스처 · 듀얼 · 색 변화, 실시간 미리보기
//
// 프리셋(MariBrushPreset) 전부를 GUI 로 만진다. 저장은 호출자(MainWindow)가 .mbp 로 한다.
#ifndef MARI_UI_BRUSH_EDITOR_HPP
#define MARI_UI_BRUSH_EDITOR_HPP

#include <mari/brush/preset.hpp>

#include <QColor>
#include <QDialog>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QSpinBox;
class QTableWidget;
class QTimer;

namespace mari::ui {

class BrushEditor final : public QDialog {
    Q_OBJECT
public:
    /// `builtin` 이면 원본은 못 바꾸고 "다른 이름으로 저장" 만 된다.
    BrushEditor(const brush::MariBrushPreset& preset, bool builtin, QColor fg, QColor bg, QWidget* parent = nullptr);

    /// 편집 결과(accept 뒤).
    [[nodiscard]] const brush::MariBrushPreset& result() const noexcept { return preset_; }
    /// true 면 새 브러시로 저장(원본은 그대로).
    [[nodiscard]] bool saveAsNew() const noexcept { return saveAsNew_; }

private:
    void buildTipTab(QWidget* page);
    void buildBehaviorTab(QWidget* page);
    void buildDynamicsTab(QWidget* page);
    void buildTextureTab(QWidget* page);
    void buildDualTab(QWidget* page);
    void buildColorTab(QWidget* page);

    void loadToWidgets();
    void readFromWidgets();
    void schedulePreview();
    void refreshPreview();
    void addDynamicRow(const brush::DynamicLink& link);
    [[nodiscard]] bool loadGrayFromFile(brush::GrayImage& out, bool tipMask);

    brush::MariBrushPreset preset_;
    bool builtin_ = false;
    bool saveAsNew_ = false;
    bool loading_ = false;
    QColor fg_, bg_;

    QLabel* preview_ = nullptr;
    QTimer* previewTimer_ = nullptr;
    QLineEdit* name_ = nullptr;

    // 팁
    QComboBox* tipKind_ = nullptr;
    QLabel* tipImage_ = nullptr;
    QDoubleSpinBox* diameter_ = nullptr;
    QSpinBox* hardness_ = nullptr;
    QSpinBox* aspect_ = nullptr;
    QSpinBox* angle_ = nullptr;
    QSpinBox* spacing_ = nullptr;
    // 동작
    QSpinBox* opacity_ = nullptr;
    QSpinBox* flow_ = nullptr;
    QComboBox* blend_ = nullptr;
    QSpinBox* count_ = nullptr;
    QCheckBox* wet_ = nullptr;
    QSpinBox* noise_ = nullptr;
    QCheckBox* airbrush_ = nullptr;
    // 동적
    QTableWidget* dyn_ = nullptr;
    // 텍스처
    QCheckBox* texOn_ = nullptr;
    QLabel* texImage_ = nullptr;
    QSpinBox* texScale_ = nullptr;
    QSpinBox* texDepth_ = nullptr;
    QComboBox* texBlend_ = nullptr;
    QCheckBox* texAnchored_ = nullptr;
    // 듀얼
    QCheckBox* dualOn_ = nullptr;
    QComboBox* dualKind_ = nullptr;
    QLabel* dualImage_ = nullptr;
    QDoubleSpinBox* dualDiameter_ = nullptr;
    QSpinBox* dualHardness_ = nullptr;
    QSpinBox* dualAspect_ = nullptr;
    QSpinBox* dualAngle_ = nullptr;
    QSpinBox* dualScatter_ = nullptr;
    QSpinBox* dualCount_ = nullptr;
    QComboBox* dualBlend_ = nullptr;
    // 색
    QSpinBox* fgBg_ = nullptr;
    QSpinBox* hue_ = nullptr;
    QSpinBox* sat_ = nullptr;
    QSpinBox* bri_ = nullptr;
    QSpinBox* purity_ = nullptr;
    QCheckBox* perTip_ = nullptr;
};

} // namespace mari::ui

#endif // MARI_UI_BRUSH_EDITOR_HPP
