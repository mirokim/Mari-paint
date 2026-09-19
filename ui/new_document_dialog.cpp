// Mari Paint — 새 문서 대화상자 구현 (ui/new_document_dialog.hpp)
#include "new_document_dialog.hpp"

#include "icons.hpp"

#include <QColorDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QPushButton>
#include <QRadioButton>
#include <QResizeEvent>
#include <QSettings>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

namespace mari::ui {

namespace {

constexpr int kMaxSide = 16384;

// 첫 줄은 "사용자 지정"(직접 입력). 나머지는 px 로 고정한 흔한 규격 — 종이는 300dpi 기준.
const NewDocumentDialog::Template* templates(int& count) {
    static const NewDocumentDialog::Template t[] = {
        {"사용자 지정", 0, 0, "직접 입력"},
        {"FHD 1920 × 1080", 1920, 1080, "16:9 화면"},
        {"4K 3840 × 2160", 3840, 2160, "16:9 화면"},
        {"정사각 2000 × 2000", 2000, 2000, ""},
        {"A4 세로 (300dpi)", 2480, 3508, "210 × 297 mm"},
        {"A4 가로 (300dpi)", 3508, 2480, "297 × 210 mm"},
        {"A5 세로 (300dpi)", 1748, 2480, "148 × 210 mm"},
        {"B5 세로 (300dpi)", 2079, 2953, "176 × 250 mm"},
        {"A3 세로 (300dpi)", 3508, 4961, "297 × 420 mm"},
        {"엽서 (300dpi)", 1181, 1748, "100 × 148 mm"},
        {"인스타그램 정사각", 1080, 1080, "1:1"},
        {"인스타그램 세로", 1080, 1350, "4:5"},
        {"스토리 · 릴스", 1080, 1920, "9:16"},
        {"웹툰 한 컷", 800, 1280, "세로 스크롤 규격"},
        {"유튜브 썸네일", 1280, 720, "16:9"},
    };
    count = static_cast<int>(sizeof(t) / sizeof(t[0]));
    return t;
}

QString swatchStyle(const QColor& c) {
    return QString("QToolButton { background: %1; border: 1px solid #666; border-radius: 3px; min-width: 22px; "
                   "min-height: 22px; }")
        .arg(c.name());
}

} // namespace

NewDocumentDialog::NewDocumentDialog(QWidget* parent, bool landing) : QDialog(parent) {
    setWindowTitle(landing ? "Mari Paint — 새 문서" : "새 문서");
    setModal(true);
    resize(640, 420);

    auto* outer = new QHBoxLayout(this);
    outer->setSpacing(12);

    // ── 왼쪽: 템플릿 ─────────────────────────────────────────────────────
    list_ = new QListWidget(this);
    list_->setMinimumWidth(220);
    int n = 0;
    const Template* t = templates(n);
    for (int i = 0; i < n; ++i) {
        auto* it = new QListWidgetItem(QString::fromUtf8(t[i].name), list_);
        if (t[i].note[0] != '\0') it->setToolTip(QString::fromUtf8(t[i].note));
    }
    outer->addWidget(list_, 0);

    // ── 오른쪽: 크기 · 배경 · 미리보기 ───────────────────────────────────
    auto* right = new QVBoxLayout();
    right->setSpacing(10);
    auto* form = new QFormLayout();
    form->setLabelAlignment(Qt::AlignRight);

    auto* sizeRow = new QHBoxLayout();
    width_ = new QSpinBox(this);
    width_->setRange(1, kMaxSide);
    width_->setSuffix(" px");
    height_ = new QSpinBox(this);
    height_->setRange(1, kMaxSide);
    height_->setSuffix(" px");
    auto* swapBtn = new QToolButton(this);
    swapBtn->setIcon(themedIcon("arrows-exchange", 16));
    swapBtn->setToolTip("가로 ↔ 세로");
    swapBtn->setAutoRaise(true);
    sizeRow->addWidget(width_);
    sizeRow->addWidget(new QLabel("×", this));
    sizeRow->addWidget(height_);
    sizeRow->addWidget(swapBtn);
    sizeRow->addStretch(1);
    form->addRow("크기", sizeRow);

    auto* bgRow = new QHBoxLayout();
    bgWhite_ = new QRadioButton("흰색", this);
    bgTransparent_ = new QRadioButton("투명", this);
    bgCustom_ = new QRadioButton("색:", this);
    customSwatch_ = new QToolButton(this);
    customSwatch_->setToolTip("배경색 고르기");
    bgRow->addWidget(bgWhite_);
    bgRow->addWidget(bgTransparent_);
    bgRow->addWidget(bgCustom_);
    bgRow->addWidget(customSwatch_);
    bgRow->addStretch(1);
    form->addRow("배경", bgRow);
    right->addLayout(form);

    preview_ = new QLabel(this);
    preview_->setMinimumSize(200, 160);
    preview_->setAlignment(Qt::AlignCenter);
    preview_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    right->addWidget(preview_, 1);
    info_ = new QLabel(this);
    info_->setAlignment(Qt::AlignCenter);
    info_->setStyleSheet("color: #9a9a9a;");
    right->addWidget(info_);

    auto* buttons = new QDialogButtonBox(this);
    QPushButton* ok = buttons->addButton(landing ? "시작" : "만들기", QDialogButtonBox::AcceptRole);
    ok->setDefault(true);
    buttons->addButton(QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    right->addWidget(buttons);
    outer->addLayout(right, 1);

    // ── 마지막 값 복원 ────────────────────────────────────────────────────
    QSettings st;
    width_->setValue(std::clamp(st.value("newdoc/width", 1920).toInt(), 1, kMaxSide));
    height_->setValue(std::clamp(st.value("newdoc/height", 1080).toInt(), 1, kMaxSide));
    const int bg = st.value("newdoc/bg", 0).toInt(); // 0 흰색 · 1 투명 · 2 색
    custom_ = QColor(st.value("newdoc/customColor", custom_.name()).toString());
    if (!custom_.isValid()) custom_ = QColor(0xf0, 0xe6, 0xd2);
    (bg == 1 ? bgTransparent_ : bg == 2 ? bgCustom_ : bgWhite_)->setChecked(true);
    customSwatch_->setStyleSheet(swatchStyle(custom_));

    // ── 연결 ─────────────────────────────────────────────────────────────
    connect(list_, &QListWidget::currentRowChanged, this, &NewDocumentDialog::applyTemplate);
    connect(list_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem*) { accept(); });
    auto onSize = [this] {
        if (syncing_) return;
        syncTemplateSelection();
        updatePreview();
    };
    connect(width_, qOverload<int>(&QSpinBox::valueChanged), this, onSize);
    connect(height_, qOverload<int>(&QSpinBox::valueChanged), this, onSize);
    connect(swapBtn, &QToolButton::clicked, this, [this] {
        const int w = width_->value();
        syncing_ = true;
        width_->setValue(height_->value());
        syncing_ = false;
        height_->setValue(w);
    });
    for (QRadioButton* r : {bgWhite_, bgTransparent_, bgCustom_})
        connect(r, &QRadioButton::toggled, this, [this](bool) { updatePreview(); });
    connect(customSwatch_, &QToolButton::clicked, this, &NewDocumentDialog::pickCustomColor);
    connect(this, &QDialog::accepted, this, [this] {
        QSettings s;
        s.setValue("newdoc/width", width_->value());
        s.setValue("newdoc/height", height_->value());
        s.setValue("newdoc/bg", bgTransparent_->isChecked() ? 1 : bgCustom_->isChecked() ? 2 : 0);
        s.setValue("newdoc/customColor", custom_.name());
    });

    syncTemplateSelection();
    updatePreview();
    width_->setFocus();
    width_->selectAll();
}

int NewDocumentDialog::canvasWidth() const { return width_->value(); }
int NewDocumentDialog::canvasHeight() const { return height_->value(); }

std::optional<Color8> NewDocumentDialog::background() const {
    if (bgTransparent_->isChecked()) return std::nullopt;
    const QColor c = bgCustom_->isChecked() ? custom_ : QColor(255, 255, 255);
    return Color8::rgba(static_cast<u8>(c.red()), static_cast<u8>(c.green()), static_cast<u8>(c.blue()), 255);
}

void NewDocumentDialog::applyTemplate(int row) {
    if (syncing_ || row <= 0) return; // 0 = 사용자 지정: 값을 건드리지 않는다
    int n = 0;
    const Template* t = templates(n);
    if (row >= n) return;
    syncing_ = true;
    width_->setValue(t[row].w);
    height_->setValue(t[row].h);
    syncing_ = false;
    updatePreview();
}

void NewDocumentDialog::syncTemplateSelection() {
    // 입력한 크기가 어떤 템플릿과 같으면 그 줄을, 아니면 "사용자 지정"을 켠다.
    int n = 0;
    const Template* t = templates(n);
    int match = 0;
    for (int i = 1; i < n; ++i) {
        if (t[i].w == width_->value() && t[i].h == height_->value()) {
            match = i;
            break;
        }
    }
    syncing_ = true;
    list_->setCurrentRow(match);
    syncing_ = false;
}

void NewDocumentDialog::pickCustomColor() {
    const QColor c = QColorDialog::getColor(custom_, this, "배경색");
    if (!c.isValid()) return;
    custom_ = c;
    customSwatch_->setStyleSheet(swatchStyle(custom_));
    bgCustom_->setChecked(true);
    updatePreview();
}

void NewDocumentDialog::resizeEvent(QResizeEvent* e) {
    QDialog::resizeEvent(e);
    updatePreview();
}

void NewDocumentDialog::updatePreview() {
    const int w = width_->value(), h = height_->value();
    const QSize box = preview_->size().boundedTo(QSize(320, 240)) - QSize(16, 16);
    const qreal scale = std::min(static_cast<qreal>(std::max(box.width(), 40)) / w,
                                 static_cast<qreal>(std::max(box.height(), 40)) / h);
    const int pw = std::max(8, static_cast<int>(w * scale));
    const int ph = std::max(8, static_cast<int>(h * scale));
    QPixmap pm(pw + 2, ph + 2);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    const std::optional<Color8> bg = background();
    if (bg.has_value()) {
        p.fillRect(1, 1, pw, ph, QColor(bg->r, bg->g, bg->b));
    } else {
        // 투명은 체크무늬로
        for (int y = 0; y < ph; y += 8)
            for (int x = 0; x < pw; x += 8)
                p.fillRect(1 + x, 1 + y, std::min(8, pw - x), std::min(8, ph - y),
                           ((x / 8 + y / 8) % 2) ? QColor(0xa8, 0xa8, 0xa8) : QColor(0xd8, 0xd8, 0xd8));
    }
    p.setPen(QColor(0x55, 0x55, 0x55));
    p.drawRect(0, 0, pw + 1, ph + 1);
    p.end();
    preview_->setPixmap(pm);
    const double mp = static_cast<double>(w) * h / 1'000'000.0;
    info_->setText(QString("%1 × %2 px · %3 MP · 가로세로 %4")
                       .arg(w)
                       .arg(h)
                       .arg(mp, 0, 'f', 1)
                       .arg(static_cast<double>(w) / h, 0, 'f', 2));
}

} // namespace mari::ui
