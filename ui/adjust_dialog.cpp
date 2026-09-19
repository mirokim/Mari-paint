// Mari Paint — 색 보정 대화상자 구현 (ui/adjust_dialog.hpp)
#include "adjust_dialog.hpp"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

namespace mari::ui {

// ── CurveWidget ───────────────────────────────────────────────────────────

CurveWidget::CurveWidget(QWidget* parent) : QWidget(parent) {
    setFixedSize(256, 256);
    setMouseTracking(true);
}

void CurveWidget::setPoints(std::vector<app::CurvePt> p) {
    pts_ = std::move(p);
    if (pts_.size() < 2) pts_ = {{0.0f, 0.0f}, {1.0f, 1.0f}};
    update();
}

void CurveWidget::reset() {
    pts_ = {{0.0f, 0.0f}, {1.0f, 1.0f}};
    update();
    Q_EMIT changed();
}

QPointF CurveWidget::toWidget(const app::CurvePt& p) const {
    return QPointF(p.x * (width() - 1), (1.0f - p.y) * (height() - 1));
}

app::CurvePt CurveWidget::fromWidget(const QPointF& q) const {
    return {std::clamp(static_cast<float>(q.x() / (width() - 1)), 0.0f, 1.0f),
            std::clamp(static_cast<float>(1.0 - q.y() / (height() - 1)), 0.0f, 1.0f)};
}

void CurveWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), QColor(0x2b, 0x2b, 0x2b));
    p.setPen(QColor(0x44, 0x44, 0x44));
    for (int i = 1; i < 4; ++i) {
        p.drawLine(i * width() / 4, 0, i * width() / 4, height());
        p.drawLine(0, i * height() / 4, width(), i * height() / 4);
    }
    p.setPen(QColor(0x66, 0x66, 0x66));
    p.drawLine(0, height() - 1, width() - 1, 0);
    p.setRenderHint(QPainter::Antialiasing, true);
    // 구간 선형(엔진과 같은 해석).
    std::vector<app::CurvePt> sorted = pts_;
    std::sort(sorted.begin(), sorted.end(), [](const app::CurvePt& a, const app::CurvePt& b) { return a.x < b.x; });
    QPainterPath path;
    path.moveTo(toWidget({0.0f, sorted.front().y}));
    for (const app::CurvePt& c : sorted) path.lineTo(toWidget(c));
    path.lineTo(toWidget({1.0f, sorted.back().y}));
    p.setPen(QPen(QColor(0x3d, 0x7b, 0xd9), 2.0));
    p.drawPath(path);
    p.setBrush(Qt::white);
    p.setPen(QPen(Qt::black, 1.0));
    for (const app::CurvePt& c : sorted) p.drawEllipse(toWidget(c), 4.0, 4.0);
}

void CurveWidget::mousePressEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) return;
    drag_ = -1;
    for (usize i = 0; i < pts_.size(); ++i) {
        if (QLineF(toWidget(pts_[i]), e->position()).length() <= 8.0) drag_ = static_cast<int>(i);
    }
    if (drag_ < 0 && pts_.size() < 32) {
        pts_.push_back(fromWidget(e->position()));
        drag_ = static_cast<int>(pts_.size()) - 1;
    }
    update();
}

void CurveWidget::mouseMoveEvent(QMouseEvent* e) {
    if (drag_ < 0) return;
    app::CurvePt c = fromWidget(e->position());
    // 양 끝점은 x 를 고정한다.
    if (pts_[static_cast<usize>(drag_)].x <= 0.0f || pts_[static_cast<usize>(drag_)].x >= 1.0f) c.x = pts_[static_cast<usize>(drag_)].x;
    pts_[static_cast<usize>(drag_)] = c;
    update();
    Q_EMIT changed();
}

void CurveWidget::mouseReleaseEvent(QMouseEvent*) {
    if (drag_ >= 0) Q_EMIT changed();
    drag_ = -1;
}

void CurveWidget::mouseDoubleClickEvent(QMouseEvent* e) {
    for (usize i = 0; i < pts_.size(); ++i) {
        if (QLineF(toWidget(pts_[i]), e->position()).length() <= 8.0 && pts_[i].x > 0.0f && pts_[i].x < 1.0f) {
            pts_.erase(pts_.begin() + static_cast<long long>(i));
            update();
            Q_EMIT changed();
            return;
        }
    }
}

// ── AdjustDialog ──────────────────────────────────────────────────────────

AdjustDialog::AdjustDialog(app::AdjustKind kind, std::function<bool(const app::AdjustParams&)> applyFn,
                           std::function<void()> undoFn, QWidget* parent)
    : QDialog(parent), apply_(std::move(applyFn)), undo_(std::move(undoFn)) {
    params_.kind = kind;
    auto* layout = new QVBoxLayout(this);
    auto* page = new QWidget(this);
    switch (kind) {
    case app::AdjustKind::HueSaturation: setWindowTitle("색조/채도"); buildHsl(page); break;
    case app::AdjustKind::BrightnessContrast: setWindowTitle("밝기/대비"); buildBrightness(page); break;
    case app::AdjustKind::Levels: setWindowTitle("레벨"); buildLevels(page); break;
    case app::AdjustKind::Curves: setWindowTitle("곡선"); buildCurves(page); break;
    case app::AdjustKind::Threshold: setWindowTitle("문턱값"); buildThreshold(page); break;
    case app::AdjustKind::Posterize: setWindowTitle("포스터화"); buildPosterize(page); break;
    default: setWindowTitle("보정"); break;
    }
    layout->addWidget(page);
    previewOn_ = new QCheckBox("미리보기", this);
    previewOn_->setChecked(true);
    layout->addWidget(previewOn_);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::Reset, this);
    buttons->button(QDialogButtonBox::Ok)->setText("확인");
    buttons->button(QDialogButtonBox::Cancel)->setText("취소");
    buttons->button(QDialogButtonBox::Reset)->setText("초기화");
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons->button(QDialogButtonBox::Reset), &QPushButton::clicked, this, [this] {
        loading_ = true;
        for (QSlider* s : {hue_, sat_, light_, bright_, contrast_, inB_, outB_}) if (s) s->setValue(0);
        for (QSlider* s : {inW_, outW_}) if (s) s->setValue(255);
        if (gamma_) gamma_->setValue(1.0);
        if (curve_) curve_->reset();
        if (threshold_) threshold_->setValue(128);
        if (levels_) levels_->setValue(4);
        loading_ = false;
        schedule();
    });
    timer_ = new QTimer(this);
    timer_->setSingleShot(true);
    timer_->setInterval(40);
    connect(timer_, &QTimer::timeout, this, &AdjustDialog::applyPreview);
    connect(previewOn_, &QCheckBox::toggled, this, [this](bool on) {
        if (!on && previewApplied_) { undo_(); previewApplied_ = false; }
        else if (on) schedule();
    });
    // 문턱값·포스터화·곡선은 기본값도 이미 그림을 바꾼다 — 열자마자 미리보기.
    if (kind == app::AdjustKind::Threshold || kind == app::AdjustKind::Posterize) schedule();
}

QSlider* AdjustDialog::sliderRow(QWidget* page, QFormLayout* form, const QString& label, int lo, int hi, int value,
                                 QSpinBox** spinOut) {
    auto* row = new QWidget(page);
    auto* h = new QHBoxLayout(row);
    h->setContentsMargins(0, 0, 0, 0);
    auto* s = new QSlider(Qt::Horizontal, row);
    s->setRange(lo, hi);
    s->setValue(value);
    s->setMinimumWidth(220);
    auto* sp = new QSpinBox(row);
    sp->setRange(lo, hi);
    sp->setValue(value);
    sp->setFixedWidth(64);
    h->addWidget(s, 1);
    h->addWidget(sp);
    connect(s, &QSlider::valueChanged, sp, &QSpinBox::setValue);
    connect(sp, &QSpinBox::valueChanged, s, &QSlider::setValue);
    connect(s, &QSlider::valueChanged, this, [this](int) { schedule(); });
    form->addRow(label, row);
    if (spinOut) *spinOut = sp;
    return s;
}

void AdjustDialog::buildHsl(QWidget* page) {
    auto* form = new QFormLayout(page);
    hue_ = sliderRow(page, form, "색조", -180, 180, 0);
    sat_ = sliderRow(page, form, "채도", -100, 100, 0);
    light_ = sliderRow(page, form, "명도", -100, 100, 0);
}

void AdjustDialog::buildBrightness(QWidget* page) {
    auto* form = new QFormLayout(page);
    bright_ = sliderRow(page, form, "밝기", -100, 100, 0);
    contrast_ = sliderRow(page, form, "대비", -100, 100, 0);
}

void AdjustDialog::buildLevels(QWidget* page) {
    auto* form = new QFormLayout(page);
    inB_ = sliderRow(page, form, "입력 검정", 0, 255, 0);
    inW_ = sliderRow(page, form, "입력 흰색", 0, 255, 255);
    gamma_ = new QDoubleSpinBox(page);
    gamma_->setRange(0.1, 10.0);
    gamma_->setSingleStep(0.05);
    gamma_->setValue(1.0);
    connect(gamma_, &QDoubleSpinBox::valueChanged, this, [this](double) { schedule(); });
    form->addRow("감마", gamma_);
    outB_ = sliderRow(page, form, "출력 검정", 0, 255, 0);
    outW_ = sliderRow(page, form, "출력 흰색", 0, 255, 255);
}

void AdjustDialog::buildCurves(QWidget* page) {
    auto* v = new QVBoxLayout(page);
    v->addWidget(new QLabel("클릭: 점 추가 · 드래그: 이동 · 더블클릭: 삭제", page));
    curve_ = new CurveWidget(page);
    v->addWidget(curve_, 0, Qt::AlignHCenter);
    connect(curve_, &CurveWidget::changed, this, [this] { schedule(); });
}

void AdjustDialog::buildThreshold(QWidget* page) {
    auto* form = new QFormLayout(page);
    threshold_ = sliderRow(page, form, "문턱값", 0, 255, 128);
}

void AdjustDialog::buildPosterize(QWidget* page) {
    auto* form = new QFormLayout(page);
    levels_ = sliderRow(page, form, "단계", 2, 32, 4);
}

void AdjustDialog::readParams() {
    app::AdjustParams& p = params_;
    if (hue_) { p.hue = static_cast<f32>(hue_->value()); p.saturation = static_cast<f32>(sat_->value()); p.lightness = static_cast<f32>(light_->value()); }
    if (bright_) { p.brightness = static_cast<f32>(bright_->value()); p.contrast = static_cast<f32>(contrast_->value()); }
    if (inB_) { p.inBlack = inB_->value(); p.inWhite = inW_->value(); p.outBlack = outB_->value(); p.outWhite = outW_->value(); p.gamma = static_cast<f32>(gamma_->value()); }
    if (curve_) p.curve = curve_->points();
    if (threshold_) p.threshold = threshold_->value();
    if (levels_) p.levels = levels_->value();
}

void AdjustDialog::schedule() {
    if (loading_) return;
    if (previewOn_ != nullptr && !previewOn_->isChecked()) return;
    timer_->start();
}

void AdjustDialog::applyPreview() {
    readParams();
    if (previewApplied_) { undo_(); previewApplied_ = false; }
    previewApplied_ = apply_(params_);
}

void AdjustDialog::accept() {
    timer_->stop();
    readParams();
    if (previewApplied_) { undo_(); previewApplied_ = false; }
    apply_(params_); // 최종 한 번 — 실행취소 항목 하나
    QDialog::accept();
}

void AdjustDialog::reject() {
    timer_->stop();
    if (previewApplied_) { undo_(); previewApplied_ = false; }
    QDialog::reject();
}

} // namespace mari::ui
