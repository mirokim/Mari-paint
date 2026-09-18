// Mari Paint — 브러시 편집기 구현 (ui/brush_editor.hpp)
#include "brush_editor.hpp"

#include "brush_preview.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

namespace mari::ui {

using brush::BrushTip;
using brush::DynamicInput;
using brush::DynamicLink;
using brush::DynamicOutput;
using brush::GrayImage;
using brush::MariBrushPreset;
using brush::ProceduralShape;
using brush::TipKind;

namespace {

constexpr int kPreviewW = 520;
constexpr int kPreviewH = 130;
constexpr int kTipPreview = 72;

const BlendMode kStrokeBlends[] = {BlendMode::Normal, BlendMode::Multiply, BlendMode::Screen, BlendMode::Add};
const BlendMode kTextureBlends[] = {BlendMode::Multiply, BlendMode::Subtract, BlendMode::Darken, BlendMode::Screen,
                                    BlendMode::Overlay};
const BlendMode kDualBlends[] = {BlendMode::Multiply, BlendMode::Darken, BlendMode::Screen, BlendMode::Add,
                                 BlendMode::Overlay};

const char* kInputNames[] = {"필압", "기울기 X", "기울기 Y", "방위각", "속도", "난수", "방향", "페이드"};
const char* kOutputNames[] = {"크기", "불투명도", "유량", "원형도", "회전", "흩뿌림"};

QSpinBox* spin(int lo, int hi, const QString& suffix = QString()) {
    auto* s = new QSpinBox();
    s->setRange(lo, hi);
    if (!suffix.isEmpty()) s->setSuffix(suffix);
    return s;
}

QComboBox* blendCombo(const BlendMode* modes, usize n) {
    auto* c = new QComboBox();
    for (usize i = 0; i < n; ++i) c->addItem(QString::fromLatin1(blendModeName(modes[i])), static_cast<int>(modes[i]));
    return c;
}

void selectBlend(QComboBox* c, BlendMode m) {
    const int i = c->findData(static_cast<int>(m));
    c->setCurrentIndex(i < 0 ? 0 : i);
}

BlendMode blendOf(const QComboBox* c) { return static_cast<BlendMode>(c->currentData().toInt()); }

QPixmap tipPixmap(const BrushTip& tip, QColor fg, qreal dpr) {
    QImage img = renderTipPreview(tip, static_cast<int>(kTipPreview * dpr), fg);
    img.setDevicePixelRatio(dpr);
    return QPixmap::fromImage(img);
}

QPixmap grayPixmap(const GrayImage& g, int px, qreal dpr) {
    if (g.empty()) return QPixmap();
    QImage img(g.width, g.height, QImage::Format_Grayscale8);
    for (int y = 0; y < g.height; ++y) std::copy_n(g.pixels.data() + static_cast<usize>(y) * g.width, g.width, img.scanLine(y));
    QImage scaled = img.scaled(QSize(px, px) * dpr, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    scaled.setDevicePixelRatio(dpr);
    return QPixmap::fromImage(scaled);
}

} // namespace

BrushEditor::BrushEditor(const MariBrushPreset& preset, bool builtin, QColor fg, QColor bg, QWidget* parent)
    : QDialog(parent), preset_(preset), builtin_(builtin), fg_(fg), bg_(bg) {
    setWindowTitle(QString("브러시 편집 — %1").arg(QString::fromStdString(preset.name)));
    resize(760, 640);
    auto* layout = new QVBoxLayout(this);

    preview_ = new QLabel(this);
    preview_->setFixedSize(kPreviewW, kPreviewH);
    preview_->setAlignment(Qt::AlignCenter);
    preview_->setStyleSheet("background: #f2f2f2; border: 1px solid #1e1e1e;");
    layout->addWidget(preview_, 0, Qt::AlignHCenter);

    auto* nameRow = new QHBoxLayout();
    nameRow->addWidget(new QLabel("이름", this));
    name_ = new QLineEdit(QString::fromStdString(preset.name), this);
    nameRow->addWidget(name_, 1);
    layout->addLayout(nameRow);

    auto* tabs = new QTabWidget(this);
    const auto page = [&](const char* title, void (BrushEditor::*build)(QWidget*)) {
        auto* w = new QWidget(tabs);
        (this->*build)(w);
        tabs->addTab(w, title);
    };
    page("팁", &BrushEditor::buildTipTab);
    page("동작", &BrushEditor::buildBehaviorTab);
    page("동적 반응", &BrushEditor::buildDynamicsTab);
    page("텍스처", &BrushEditor::buildTextureTab);
    page("듀얼 브러시", &BrushEditor::buildDualTab);
    page("색 변화", &BrushEditor::buildColorTab);
    layout->addWidget(tabs, 1);

    auto* buttons = new QDialogButtonBox(this);
    QPushButton* save = buttons->addButton(builtin_ ? "사본으로 저장" : "저장", QDialogButtonBox::AcceptRole);
    QPushButton* saveAs = builtin_ ? nullptr : buttons->addButton("다른 이름으로 저장", QDialogButtonBox::ActionRole);
    buttons->addButton("취소", QDialogButtonBox::RejectRole);
    layout->addWidget(buttons);
    connect(save, &QPushButton::clicked, this, [this] {
        readFromWidgets();
        if (preset_.name.empty()) {
            QMessageBox::warning(this, "브러시", "이름을 넣어라.");
            return;
        }
        saveAsNew_ = builtin_;
        accept();
    });
    if (saveAs != nullptr) {
        connect(saveAs, &QPushButton::clicked, this, [this] {
            readFromWidgets();
            saveAsNew_ = true;
            if (preset_.name.empty()) preset_.name = "브러시";
            accept();
        });
    }
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    previewTimer_ = new QTimer(this);
    previewTimer_->setSingleShot(true);
    previewTimer_->setInterval(60);
    connect(previewTimer_, &QTimer::timeout, this, &BrushEditor::refreshPreview);

    loadToWidgets();
    refreshPreview();
}

// ── 탭 구성 ──────────────────────────────────────────────────────────────

void BrushEditor::buildTipTab(QWidget* page) {
    auto* row = new QHBoxLayout(page);
    auto* form = new QFormLayout();
    tipKind_ = new QComboBox(page);
    tipKind_->addItems({"원형", "사각형", "마름모", "비트맵"});
    form->addRow("모양", tipKind_);
    auto* load = new QPushButton("이미지에서 팁 불러오기…", page);
    form->addRow("", load);
    diameter_ = new QDoubleSpinBox(page);
    diameter_->setRange(0.5, 2000.0);
    diameter_->setDecimals(1);
    diameter_->setSuffix(" px");
    form->addRow("지름", diameter_);
    hardness_ = spin(0, 100, "%");
    form->addRow("경도", hardness_);
    aspect_ = spin(1, 100, "%");
    form->addRow("원형도", aspect_);
    angle_ = spin(-180, 180, "°");
    form->addRow("각도", angle_);
    spacing_ = spin(1, 1000, "%");
    form->addRow("간격", spacing_);
    row->addLayout(form, 1);
    tipImage_ = new QLabel(page);
    tipImage_->setFixedSize(kTipPreview + 8, kTipPreview + 8);
    tipImage_->setAlignment(Qt::AlignCenter);
    tipImage_->setStyleSheet("background: #f2f2f2; border: 1px solid #1e1e1e;");
    row->addWidget(tipImage_, 0, Qt::AlignTop);

    connect(load, &QPushButton::clicked, this, [this] {
        if (loadGrayFromFile(preset_.tip.bitmap, true)) {
            preset_.tip.kind = TipKind::Bitmap;
            tipKind_->setCurrentIndex(3);
            schedulePreview();
        }
    });
    const auto ch = [this] { schedulePreview(); };
    connect(tipKind_, &QComboBox::currentIndexChanged, this, [this](int i) {
        if (i == 3 && preset_.tip.bitmap.empty()) {
            if (!loadGrayFromFile(preset_.tip.bitmap, true)) tipKind_->setCurrentIndex(0);
        }
        schedulePreview();
    });
    connect(diameter_, &QDoubleSpinBox::valueChanged, this, [ch](double) { ch(); });
    for (QSpinBox* s : {hardness_, aspect_, angle_, spacing_}) connect(s, &QSpinBox::valueChanged, this, [ch](int) { ch(); });
}

void BrushEditor::buildBehaviorTab(QWidget* page) {
    auto* form = new QFormLayout(page);
    opacity_ = spin(0, 100, "%");
    form->addRow("불투명도", opacity_);
    flow_ = spin(0, 100, "%");
    form->addRow("유량", flow_);
    blend_ = blendCombo(kStrokeBlends, std::size(kStrokeBlends));
    form->addRow("합성 모드", blend_);
    count_ = spin(1, 16);
    form->addRow("스탬프 개수", count_);
    wet_ = new QCheckBox("젖은 가장자리(수채처럼 가장자리에 잉크가 고인다)", page);
    form->addRow("", wet_);
    noise_ = spin(0, 100, "%");
    form->addRow("노이즈", noise_);
    airbrush_ = new QCheckBox("에어브러시(멈춰 있어도 쌓임 — 엔진이 시간 반복을 하면 적용)", page);
    form->addRow("", airbrush_);
    const auto ch = [this] { schedulePreview(); };
    for (QSpinBox* s : {opacity_, flow_, count_, noise_}) connect(s, &QSpinBox::valueChanged, this, [ch](int) { ch(); });
    connect(blend_, &QComboBox::currentIndexChanged, this, [ch](int) { ch(); });
    connect(wet_, &QCheckBox::toggled, this, [ch](bool) { ch(); });
}

void BrushEditor::buildDynamicsTab(QWidget* page) {
    auto* layout = new QVBoxLayout(page);
    layout->addWidget(new QLabel("입력(필압·기울기·속도·난수…)이 출력(크기·불투명도·회전…)에 얼마나 반응하는가. "
                                 "'최소' 는 입력 0 일 때의 배율(크기·불투명도·유량·원형도만).", page));
    dyn_ = new QTableWidget(0, 4, page);
    dyn_->setHorizontalHeaderLabels({"입력", "출력", "세기 %", "최소 %"});
    dyn_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    dyn_->verticalHeader()->setVisible(false);
    dyn_->setSelectionBehavior(QAbstractItemView::SelectRows);
    layout->addWidget(dyn_, 1);
    auto* row = new QHBoxLayout();
    auto* add = new QPushButton("추가", page);
    auto* del = new QPushButton("삭제", page);
    auto* presetPressure = new QPushButton("필압 → 크기·불투명도 (기본)", page);
    row->addWidget(add);
    row->addWidget(del);
    row->addStretch(1);
    row->addWidget(presetPressure);
    layout->addLayout(row);
    connect(add, &QPushButton::clicked, this, [this] {
        DynamicLink l;
        l.curve.points = {{0.0f, 0.0f}, {1.0f, 1.0f}};
        addDynamicRow(l);
        schedulePreview();
    });
    connect(del, &QPushButton::clicked, this, [this] {
        const int r = dyn_->currentRow();
        if (r >= 0) dyn_->removeRow(r);
        schedulePreview();
    });
    connect(presetPressure, &QPushButton::clicked, this, [this] {
        for (DynamicOutput out : {DynamicOutput::Size, DynamicOutput::Opacity}) {
            DynamicLink l;
            l.input = DynamicInput::Pressure;
            l.output = out;
            l.curve.points = {{0.0f, out == DynamicOutput::Size ? 0.1f : 0.0f}, {1.0f, 1.0f}};
            addDynamicRow(l);
        }
        schedulePreview();
    });
}

void BrushEditor::addDynamicRow(const DynamicLink& link) {
    const int r = dyn_->rowCount();
    dyn_->insertRow(r);
    auto* in = new QComboBox();
    for (const char* n : kInputNames) in->addItem(n);
    in->setCurrentIndex(static_cast<int>(link.input));
    auto* out = new QComboBox();
    for (const char* n : kOutputNames) out->addItem(n);
    out->setCurrentIndex(static_cast<int>(link.output));
    auto* amount = spin(0, 300, "%");
    amount->setValue(static_cast<int>(link.amount * 100.0f + 0.5f));
    auto* minimum = spin(0, 100, "%");
    const bool additive = link.output == DynamicOutput::Rotation || link.output == DynamicOutput::Scatter;
    const f32 minV = (!additive && link.curve.points.size() >= 2) ? link.curve.points.front().y : 0.0f;
    minimum->setValue(static_cast<int>(minV * 100.0f + 0.5f));
    dyn_->setCellWidget(r, 0, in);
    dyn_->setCellWidget(r, 1, out);
    dyn_->setCellWidget(r, 2, amount);
    dyn_->setCellWidget(r, 3, minimum);
    const auto ch = [this] { schedulePreview(); };
    connect(in, &QComboBox::currentIndexChanged, this, [ch](int) { ch(); });
    connect(out, &QComboBox::currentIndexChanged, this, [ch](int) { ch(); });
    connect(amount, &QSpinBox::valueChanged, this, [ch](int) { ch(); });
    connect(minimum, &QSpinBox::valueChanged, this, [ch](int) { ch(); });
}

void BrushEditor::buildTextureTab(QWidget* page) {
    auto* row = new QHBoxLayout(page);
    auto* form = new QFormLayout();
    texOn_ = new QCheckBox("텍스처 사용", page);
    form->addRow("", texOn_);
    auto* load = new QPushButton("이미지 불러오기…", page);
    form->addRow("종이 결", load);
    texScale_ = spin(5, 6400, "%");
    form->addRow("배율", texScale_);
    texDepth_ = spin(0, 100, "%");
    form->addRow("깊이", texDepth_);
    texBlend_ = blendCombo(kTextureBlends, std::size(kTextureBlends));
    form->addRow("합성", texBlend_);
    texAnchored_ = new QCheckBox("캔버스에 고정(끄면 스탬프를 따라다닌다)", page);
    form->addRow("", texAnchored_);
    row->addLayout(form, 1);
    texImage_ = new QLabel(page);
    texImage_->setFixedSize(kTipPreview + 8, kTipPreview + 8);
    texImage_->setAlignment(Qt::AlignCenter);
    texImage_->setStyleSheet("background: #f2f2f2; border: 1px solid #1e1e1e;");
    row->addWidget(texImage_, 0, Qt::AlignTop);
    connect(load, &QPushButton::clicked, this, [this] {
        if (!preset_.texture.has_value()) preset_.texture = brush::BrushTexture{};
        if (loadGrayFromFile(preset_.texture->image, false)) {
            texOn_->setChecked(true);
            texImage_->setPixmap(grayPixmap(preset_.texture->image, kTipPreview, devicePixelRatioF()));
            schedulePreview();
        }
    });
    const auto ch = [this] { schedulePreview(); };
    connect(texOn_, &QCheckBox::toggled, this, [this](bool on) {
        if (on && (!preset_.texture.has_value() || preset_.texture->image.empty())) {
            if (!preset_.texture.has_value()) preset_.texture = brush::BrushTexture{};
            if (!loadGrayFromFile(preset_.texture->image, false)) {
                texOn_->setChecked(false);
                return;
            }
            texImage_->setPixmap(grayPixmap(preset_.texture->image, kTipPreview, devicePixelRatioF()));
        }
        schedulePreview();
    });
    for (QSpinBox* s : {texScale_, texDepth_}) connect(s, &QSpinBox::valueChanged, this, [ch](int) { ch(); });
    connect(texBlend_, &QComboBox::currentIndexChanged, this, [ch](int) { ch(); });
    connect(texAnchored_, &QCheckBox::toggled, this, [ch](bool) { ch(); });
}

void BrushEditor::buildDualTab(QWidget* page) {
    auto* row = new QHBoxLayout(page);
    auto* form = new QFormLayout();
    dualOn_ = new QCheckBox("듀얼 브러시 사용(두 번째 팁을 곱해 결을 만든다)", page);
    form->addRow("", dualOn_);
    dualKind_ = new QComboBox(page);
    dualKind_->addItems({"원형", "사각형", "마름모", "비트맵"});
    form->addRow("모양", dualKind_);
    auto* load = new QPushButton("이미지에서 팁 불러오기…", page);
    form->addRow("", load);
    dualDiameter_ = new QDoubleSpinBox(page);
    dualDiameter_->setRange(0.5, 2000.0);
    dualDiameter_->setDecimals(1);
    dualDiameter_->setSuffix(" px");
    form->addRow("지름", dualDiameter_);
    dualHardness_ = spin(0, 100, "%");
    form->addRow("경도", dualHardness_);
    dualAspect_ = spin(1, 100, "%");
    form->addRow("원형도", dualAspect_);
    dualAngle_ = spin(-180, 180, "°");
    form->addRow("각도", dualAngle_);
    dualScatter_ = spin(0, 500, "%");
    form->addRow("흩뿌림", dualScatter_);
    dualCount_ = spin(1, 16);
    form->addRow("개수", dualCount_);
    dualBlend_ = blendCombo(kDualBlends, std::size(kDualBlends));
    form->addRow("합성", dualBlend_);
    row->addLayout(form, 1);
    dualImage_ = new QLabel(page);
    dualImage_->setFixedSize(kTipPreview + 8, kTipPreview + 8);
    dualImage_->setAlignment(Qt::AlignCenter);
    dualImage_->setStyleSheet("background: #f2f2f2; border: 1px solid #1e1e1e;");
    row->addWidget(dualImage_, 0, Qt::AlignTop);
    connect(load, &QPushButton::clicked, this, [this] {
        if (!preset_.dual.has_value()) preset_.dual = brush::DualBrush{};
        if (loadGrayFromFile(preset_.dual->tip.bitmap, true)) {
            preset_.dual->tip.kind = TipKind::Bitmap;
            dualKind_->setCurrentIndex(3);
            dualOn_->setChecked(true);
            schedulePreview();
        }
    });
    const auto ch = [this] { schedulePreview(); };
    connect(dualOn_, &QCheckBox::toggled, this, [ch](bool) { ch(); });
    connect(dualKind_, &QComboBox::currentIndexChanged, this, [this](int i) {
        if (i == 3 && (!preset_.dual.has_value() || preset_.dual->tip.bitmap.empty())) {
            if (!preset_.dual.has_value()) preset_.dual = brush::DualBrush{};
            if (!loadGrayFromFile(preset_.dual->tip.bitmap, true)) dualKind_->setCurrentIndex(0);
        }
        schedulePreview();
    });
    connect(dualDiameter_, &QDoubleSpinBox::valueChanged, this, [ch](double) { ch(); });
    for (QSpinBox* s : {dualHardness_, dualAspect_, dualAngle_, dualScatter_, dualCount_})
        connect(s, &QSpinBox::valueChanged, this, [ch](int) { ch(); });
    connect(dualBlend_, &QComboBox::currentIndexChanged, this, [ch](int) { ch(); });
}

void BrushEditor::buildColorTab(QWidget* page) {
    auto* form = new QFormLayout(page);
    fgBg_ = spin(0, 100, "%");
    form->addRow("전경↔배경 지터", fgBg_);
    hue_ = spin(0, 100, "%");
    form->addRow("색조 지터", hue_);
    sat_ = spin(0, 100, "%");
    form->addRow("채도 지터", sat_);
    bri_ = spin(0, 100, "%");
    form->addRow("명도 지터", bri_);
    purity_ = spin(-100, 100, "%");
    form->addRow("순도", purity_);
    perTip_ = new QCheckBox("스탬프마다(끄면 획마다 한 번)", page);
    form->addRow("", perTip_);
    const auto ch = [this] { schedulePreview(); };
    for (QSpinBox* s : {fgBg_, hue_, sat_, bri_, purity_}) connect(s, &QSpinBox::valueChanged, this, [ch](int) { ch(); });
    connect(perTip_, &QCheckBox::toggled, this, [ch](bool) { ch(); });
}

// ── 값 왕복 ──────────────────────────────────────────────────────────────

void BrushEditor::loadToWidgets() {
    loading_ = true;
    const MariBrushPreset& p = preset_;
    const int kindIdx = p.tip.kind == TipKind::Bitmap ? 3 : static_cast<int>(p.tip.shape);
    tipKind_->setCurrentIndex(kindIdx);
    diameter_->setValue(p.tip.diameter);
    hardness_->setValue(static_cast<int>(p.tip.hardness * 100.0f + 0.5f));
    aspect_->setValue(static_cast<int>(p.tip.aspectRatio * 100.0f + 0.5f));
    angle_->setValue(static_cast<int>(p.tip.angle));
    spacing_->setValue(static_cast<int>(p.spacing * 100.0f + 0.5f));

    opacity_->setValue(static_cast<int>(p.opacity * 100.0f + 0.5f));
    flow_->setValue(static_cast<int>(p.flow * 100.0f + 0.5f));
    selectBlend(blend_, p.blendMode);
    count_->setValue(p.scatterCount);
    wet_->setChecked(p.wetEdges);
    noise_->setValue(static_cast<int>(p.noise * 100.0f + 0.5f));
    airbrush_->setChecked(p.airbrush);

    dyn_->setRowCount(0);
    for (const DynamicLink& l : p.dynamics) addDynamicRow(l);

    texOn_->setChecked(p.texture.has_value() && !p.texture->image.empty());
    if (p.texture.has_value()) {
        texScale_->setValue(static_cast<int>(p.texture->scale * 100.0f + 0.5f));
        texDepth_->setValue(static_cast<int>(p.texture->depth * 100.0f + 0.5f));
        selectBlend(texBlend_, p.texture->blendMode);
        texAnchored_->setChecked(p.texture->anchoredToCanvas);
        texImage_->setPixmap(grayPixmap(p.texture->image, kTipPreview, devicePixelRatioF()));
    } else {
        texScale_->setValue(100);
        texDepth_->setValue(100);
        texAnchored_->setChecked(true);
    }

    dualOn_->setChecked(p.dual.has_value());
    if (p.dual.has_value()) {
        const brush::DualBrush& d = *p.dual;
        dualKind_->setCurrentIndex(d.tip.kind == TipKind::Bitmap ? 3 : static_cast<int>(d.tip.shape));
        dualDiameter_->setValue(d.tip.diameter);
        dualHardness_->setValue(static_cast<int>(d.tip.hardness * 100.0f + 0.5f));
        dualAspect_->setValue(static_cast<int>(d.tip.aspectRatio * 100.0f + 0.5f));
        dualAngle_->setValue(static_cast<int>(d.tip.angle));
        dualScatter_->setValue(static_cast<int>(d.scatter * 100.0f + 0.5f));
        dualCount_->setValue(d.count);
        selectBlend(dualBlend_, d.blendMode);
    } else {
        dualDiameter_->setValue(p.tip.diameter * 0.5);
        dualHardness_->setValue(100);
        dualAspect_->setValue(100);
        dualCount_->setValue(1);
    }

    fgBg_->setValue(static_cast<int>(p.colorDynamics.fgBgJitter * 100.0f + 0.5f));
    hue_->setValue(static_cast<int>(p.colorDynamics.hueJitter * 100.0f + 0.5f));
    sat_->setValue(static_cast<int>(p.colorDynamics.saturationJitter * 100.0f + 0.5f));
    bri_->setValue(static_cast<int>(p.colorDynamics.brightnessJitter * 100.0f + 0.5f));
    purity_->setValue(static_cast<int>(p.colorDynamics.purity * 100.0f));
    perTip_->setChecked(p.colorDynamics.perTip);
    loading_ = false;
}

void BrushEditor::readFromWidgets() {
    MariBrushPreset& p = preset_;
    p.name = name_->text().trimmed().toStdString();
    const int k = tipKind_->currentIndex();
    if (k == 3 && !p.tip.bitmap.empty()) {
        p.tip.kind = TipKind::Bitmap;
    } else {
        p.tip.kind = TipKind::Procedural;
        p.tip.shape = static_cast<ProceduralShape>(std::clamp(k, 0, 2));
    }
    p.tip.diameter = static_cast<f32>(diameter_->value());
    p.tip.hardness = hardness_->value() / 100.0f;
    p.tip.aspectRatio = aspect_->value() / 100.0f;
    p.tip.angle = static_cast<f32>(angle_->value());
    p.spacing = spacing_->value() / 100.0f;

    p.opacity = opacity_->value() / 100.0f;
    p.flow = flow_->value() / 100.0f;
    p.blendMode = blendOf(blend_);
    p.scatterCount = count_->value();
    p.wetEdges = wet_->isChecked();
    p.noise = noise_->value() / 100.0f;
    p.airbrush = airbrush_->isChecked();

    std::vector<DynamicLink> links;
    for (int r = 0; r < dyn_->rowCount(); ++r) {
        auto* in = static_cast<QComboBox*>(dyn_->cellWidget(r, 0));
        auto* out = static_cast<QComboBox*>(dyn_->cellWidget(r, 1));
        auto* amount = static_cast<QSpinBox*>(dyn_->cellWidget(r, 2));
        auto* minimum = static_cast<QSpinBox*>(dyn_->cellWidget(r, 3));
        if (in == nullptr || out == nullptr) continue;
        DynamicLink l;
        l.input = static_cast<DynamicInput>(in->currentIndex());
        l.output = static_cast<DynamicOutput>(out->currentIndex());
        l.amount = amount->value() / 100.0f;
        const bool additive = l.output == DynamicOutput::Rotation || l.output == DynamicOutput::Scatter;
        // 원래 곡선이 3점 이상(임포트한 커스텀 커브)이면 그대로 둔다 — 같은 행이면.
        if (r < static_cast<int>(preset_.dynamics.size()) && preset_.dynamics[static_cast<usize>(r)].curve.points.size() > 2 &&
            preset_.dynamics[static_cast<usize>(r)].input == l.input && preset_.dynamics[static_cast<usize>(r)].output == l.output) {
            l.curve = preset_.dynamics[static_cast<usize>(r)].curve;
        } else if (additive) {
            l.curve.points = {{0.0f, 0.0f}, {1.0f, 1.0f}};
        } else {
            l.curve.points = {{0.0f, minimum->value() / 100.0f}, {1.0f, 1.0f}};
        }
        links.push_back(std::move(l));
    }
    p.dynamics = std::move(links);

    if (texOn_->isChecked() && p.texture.has_value() && !p.texture->image.empty()) {
        p.texture->scale = texScale_->value() / 100.0f;
        p.texture->depth = texDepth_->value() / 100.0f;
        p.texture->blendMode = blendOf(texBlend_);
        p.texture->anchoredToCanvas = texAnchored_->isChecked();
    } else if (!texOn_->isChecked()) {
        p.texture.reset();
    }

    if (dualOn_->isChecked()) {
        if (!p.dual.has_value()) p.dual = brush::DualBrush{};
        brush::DualBrush& d = *p.dual;
        const int dk = dualKind_->currentIndex();
        if (dk == 3 && !d.tip.bitmap.empty()) {
            d.tip.kind = TipKind::Bitmap;
        } else {
            d.tip.kind = TipKind::Procedural;
            d.tip.shape = static_cast<ProceduralShape>(std::clamp(dk, 0, 2));
        }
        d.tip.diameter = static_cast<f32>(dualDiameter_->value());
        d.tip.hardness = dualHardness_->value() / 100.0f;
        d.tip.aspectRatio = dualAspect_->value() / 100.0f;
        d.tip.angle = static_cast<f32>(dualAngle_->value());
        d.scatter = dualScatter_->value() / 100.0f;
        d.count = dualCount_->value();
        d.blendMode = blendOf(dualBlend_);
    } else {
        p.dual.reset();
    }

    p.colorDynamics.fgBgJitter = fgBg_->value() / 100.0f;
    p.colorDynamics.hueJitter = hue_->value() / 100.0f;
    p.colorDynamics.saturationJitter = sat_->value() / 100.0f;
    p.colorDynamics.brightnessJitter = bri_->value() / 100.0f;
    p.colorDynamics.purity = purity_->value() / 100.0f;
    p.colorDynamics.perTip = perTip_->isChecked();
}

bool BrushEditor::loadGrayFromFile(GrayImage& out, bool tipMask) {
    const QString path = QFileDialog::getOpenFileName(this, tipMask ? "팁 이미지" : "텍스처 이미지", QString(),
                                                      "이미지 (*.png *.jpg *.jpeg *.bmp *.webp)");
    if (path.isEmpty()) return false;
    QImage img(path);
    if (img.isNull()) {
        QMessageBox::warning(this, "브러시", "이미지를 읽지 못했다.");
        return false;
    }
    // 큰 이미지는 팁이면 512, 텍스처면 1024 로 줄인다(엔진 샘플 비용).
    const int cap = tipMask ? 512 : 1024;
    if (img.width() > cap || img.height() > cap) img = img.scaled(cap, cap, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    const bool useAlpha = tipMask && img.hasAlphaChannel();
    img = img.convertToFormat(useAlpha ? QImage::Format_ARGB32 : QImage::Format_Grayscale8);
    out.width = img.width();
    out.height = img.height();
    out.pixels.assign(static_cast<usize>(out.width) * static_cast<usize>(out.height), 0);
    for (int y = 0; y < img.height(); ++y) {
        for (int x = 0; x < img.width(); ++x) {
            u8 v;
            if (useAlpha) {
                v = static_cast<u8>(qAlpha(img.pixel(x, y)));
            } else {
                const u8 g = img.constScanLine(y)[x];
                v = tipMask ? static_cast<u8>(255 - g) : g; // 팁: 검을수록 잉크. 텍스처: 밝을수록 잘 묻는다
            }
            out.pixels[static_cast<usize>(y) * out.width + static_cast<usize>(x)] = v;
        }
    }
    return true;
}

void BrushEditor::schedulePreview() {
    if (!loading_) previewTimer_->start();
}

void BrushEditor::refreshPreview() {
    readFromWidgets();
    const qreal dpr = devicePixelRatioF();
    QImage img = renderBrushPreview(preset_, QSize(kPreviewW, kPreviewH) * dpr, fg_, bg_, true, false);
    img.setDevicePixelRatio(dpr);
    preview_->setPixmap(QPixmap::fromImage(img));
    tipImage_->setPixmap(tipPixmap(preset_.tip, fg_, dpr));
    if (preset_.dual.has_value()) dualImage_->setPixmap(tipPixmap(preset_.dual->tip, fg_, dpr));
    else dualImage_->clear();
}

} // namespace mari::ui
