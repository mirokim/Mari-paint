// Mari Paint — 최소 UI 구현 (ui/main_window.hpp)
#include "main_window.hpp"

#include "canvas_widget.hpp"

#include <mari/brush/builtin.hpp>
#include <mari/win/input/pointer_input.hpp>

#include <QAction>
#include <QCloseEvent>
#include <QColorDialog>
#include <QComboBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QInputDialog>
#include <QKeySequence>
#include <QLabel>
#include <QListWidget>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QStatusBar>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>

namespace mari::ui {

namespace {

QString layerLabel(const Layer& l) {
    return QString::fromStdString(l.name());
}

} // namespace

MainWindow::MainWindow(app::Application& app, QString journalPath, QWidget* parent)
    : QMainWindow(parent), app_(app), journalPath_(std::move(journalPath)) {
    brushes_ = brush::builtinPresets();

    canvas_ = new CanvasWidget(this);
    setCentralWidget(canvas_);
    canvas_->setStrokeConfigProvider([this] { return strokeConfig(); });

    buildMenus();
    buildToolbar();
    buildLayerPanel();
    buildStatusBar();

    connect(canvas_, &CanvasWidget::strokeFinished, this,
            [this](const app::LiveStrokeOutcome& o) {
                ++strokes_;
                if (o.recorded) {
                    ++recordedStrokes_;
                }
                if (o.rolledBack) {
                    ++rolledBack_;
                    statusBar()->showMessage(
                        "🔴 저널에 기록하지 못해 이 획을 되돌렸다 (docs/06 결정 ④)", 8000);
                } else if (!o.undoComplete) {
                    statusBar()->showMessage("⚠️ 실행취소 범위가 칠한 범위를 다 덮지 못했다", 5000);
                } else if (!o.engineNote.empty()) {
                    statusBar()->showMessage(QString::fromStdString(o.engineNote), 5000);
                }
                refreshStatus();
                refreshTitle();
            });
    connect(canvas_, &CanvasWidget::strokeRefused, this, [this](const QString& why) {
        statusBar()->showMessage("획을 시작하지 못했다: " + why, 6000);
    });
    connect(canvas_, &CanvasWidget::latencyUpdated, this, &MainWindow::refreshStatus);
    connect(canvas_, &CanvasWidget::viewChanged, this, &MainWindow::refreshStatus);

    attachDocument(activeDocument());
    resize(1400, 900);
    refreshTitle();
    refreshStatus();
}

MainWindow::~MainWindow() = default;

// ── 구성 ─────────────────────────────────────────────────────────────────

void MainWindow::buildMenus() {
    QMenu* file = menuBar()->addMenu("파일(&F)");
    file->addAction("새 문서(&N)...", QKeySequence::New, this, &MainWindow::newDocument);
    file->addAction("열기(&O)...", QKeySequence::Open, this, &MainWindow::openDocument);
    file->addAction("저장(&S)", QKeySequence::Save, this, [this] { saveDocument(false); });
    file->addAction("다른 이름으로 저장(&A)...", QKeySequence::SaveAs, this,
                    [this] { saveDocument(true); });
    file->addSeparator();
    file->addAction("종료(&Q)", QKeySequence::Quit, this, &QWidget::close);

    QMenu* edit = menuBar()->addMenu("편집(&E)");
    undoAction_ = edit->addAction("실행 취소(&U)", QKeySequence::Undo, this, &MainWindow::undo);
    redoAction_ = edit->addAction("다시 실행(&R)", QKeySequence::Redo, this, &MainWindow::redo);

    QMenu* view = menuBar()->addMenu("보기(&V)");
    view->addAction("확대", QKeySequence::ZoomIn, this,
                    [this] { canvas_->zoomBy(1.25, QRectF(canvas_->rect()).center()); });
    view->addAction("축소", QKeySequence::ZoomOut, this,
                    [this] { canvas_->zoomBy(0.8, QRectF(canvas_->rect()).center()); });
    view->addAction("100%", QKeySequence(Qt::CTRL | Qt::Key_0), this, [this] { canvas_->resetView(); });
    view->addAction("창에 맞춤", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F), this,
                    [this] { canvas_->fitToView(); });
    view->addSeparator();
    view->addAction("왼쪽으로 회전  [", this, [this] { canvas_->rotateBy(-15.0); });
    view->addAction("오른쪽으로 회전  ]", this, [this] { canvas_->rotateBy(15.0); });
}

void MainWindow::buildToolbar() {
    QToolBar* tb = addToolBar("도구");
    tb->setMovable(false);

    tb->addWidget(new QLabel(" 붓 "));
    brushCombo_ = new QComboBox(tb);
    for (const brush::MariBrushPreset& p : brushes_) {
        brushCombo_->addItem(QString::fromStdString(p.name));
    }
    tb->addWidget(brushCombo_);
    connect(brushCombo_, &QComboBox::currentIndexChanged, this, [this](int i) {
        if (i >= 0 && static_cast<usize>(i) < brushes_.size()) {
            sizeSpin_->setValue(brushes_[static_cast<usize>(i)].tip.diameter);
        }
    });

    tb->addWidget(new QLabel(" 크기 "));
    sizeSpin_ = new QDoubleSpinBox(tb);
    sizeSpin_->setRange(1.0, 500.0);
    sizeSpin_->setDecimals(1);
    sizeSpin_->setSuffix(" px");
    sizeSpin_->setValue(brushes_.empty() ? 10.0 : brushes_.front().tip.diameter);
    tb->addWidget(sizeSpin_);

    tb->addWidget(new QLabel(" 보정 "));
    smoothingCombo_ = new QComboBox(tb);
    smoothingCombo_->addItems({"끔", "약함", "보통", "강함"});
    tb->addWidget(smoothingCombo_);

    tb->addSeparator();
    colorButton_ = new QToolButton(tb);
    colorButton_->setText("색");
    colorButton_->setToolButtonStyle(Qt::ToolButtonTextOnly);
    colorButton_->setStyleSheet("QToolButton { background: black; color: white; min-width: 48px; }");
    connect(colorButton_, &QToolButton::clicked, this, &MainWindow::pickColor);
    tb->addWidget(colorButton_);

    eraserAction_ = tb->addAction("지우개");
    eraserAction_->setCheckable(true);
    eraserAction_->setShortcut(QKeySequence(Qt::Key_E));

    tb->addSeparator();
    tb->addAction(undoAction_);
    tb->addAction(redoAction_);
}

void MainWindow::buildLayerPanel() {
    auto* dock = new QDockWidget("레이어", this);
    dock->setFeatures(QDockWidget::DockWidgetMovable);
    auto* box = new QWidget(dock);
    auto* layout = new QVBoxLayout(box);
    layout->setContentsMargins(4, 4, 4, 4);

    layerList_ = new QListWidget(box);
    layout->addWidget(layerList_, 1);
    connect(layerList_, &QListWidget::currentRowChanged, this, &MainWindow::onLayerRowChanged);
    connect(layerList_, &QListWidget::itemChanged, this, &MainWindow::onLayerItemChanged);

    auto* buttons = new QWidget(box);
    auto* row = new QHBoxLayout(buttons);
    row->setContentsMargins(0, 0, 0, 0);
    auto* add = new QPushButton("+", buttons);
    auto* del = new QPushButton("−", buttons);
    row->addWidget(add);
    row->addWidget(del);
    layout->addWidget(buttons);
    connect(add, &QPushButton::clicked, this, &MainWindow::addLayer);
    connect(del, &QPushButton::clicked, this, &MainWindow::removeLayer);

    dock->setWidget(box);
    dock->setMinimumWidth(200);
    addDockWidget(Qt::RightDockWidgetArea, dock);
}

void MainWindow::buildStatusBar() {
    statusInput_ = new QLabel(this);
    statusRecord_ = new QLabel(this);
    statusLatency_ = new QLabel(this);
    statusStrokes_ = new QLabel(this);
    statusView_ = new QLabel(this);
    // 🔴 입력 API 는 항상 windows-ink 다. 다른 값이 뜨면 설계 위반이다(docs/03 3절).
    statusInput_->setText(QString("입력: %1").arg(mari::win::PointerInput::inputApiName()));
    statusBar()->addWidget(statusInput_);
    statusBar()->addWidget(statusRecord_);
    statusBar()->addWidget(statusStrokes_);
    statusBar()->addPermanentWidget(statusView_);
    statusBar()->addPermanentWidget(statusLatency_);
}

// ── 문서 ─────────────────────────────────────────────────────────────────

app::Document* MainWindow::activeDocument() const {
    // Application 은 Document 만 들고 있다(unique_ptr<Document>). 브리지에서 내려 받는다.
    return static_cast<app::Document*>(app_.activeDocument());
}

void MainWindow::attachDocument(app::Document* doc) {
    canvas_->setDocument(doc);
    refreshLayerList();
    refreshTitle();
    refreshStatus();
}

void MainWindow::newDocument() {
    if (!confirmDiscard()) {
        return;
    }
    bool ok = false;
    const int w = QInputDialog::getInt(this, "새 문서", "너비(px)", 1920, 1, 16384, 1, &ok);
    if (!ok) return;
    const int h = QInputDialog::getInt(this, "새 문서", "높이(px)", 1080, 1, 16384, 1, &ok);
    if (!ok) return;

    if (app::Document* old = activeDocument()) {
        canvas_->setDocument(nullptr);
        (void)app_.closeDocument(old, false);
    }
    Result<app::IDocumentBridge*> made = app_.createDocument(w, h);
    if (!made.ok()) {
        QMessageBox::warning(this, "새 문서", QString::fromStdString(made.message()));
        attachDocument(activeDocument());
        return;
    }
    attachDocument(static_cast<app::Document*>(made.value()));
}

void MainWindow::openDocument() {
    if (!confirmDiscard()) {
        return;
    }
    const QString path = QFileDialog::getOpenFileName(this, "열기", QString(), "OpenRaster (*.ora)");
    if (path.isEmpty()) {
        return;
    }
    if (app::Document* old = activeDocument()) {
        canvas_->setDocument(nullptr);
        (void)app_.closeDocument(old, false);
    }
    Result<app::IDocumentBridge*> opened = app_.open(path.toStdString());
    if (!opened.ok()) {
        QMessageBox::warning(this, "열기", QString::fromStdString(opened.message()));
        attachDocument(activeDocument());
        return;
    }
    attachDocument(static_cast<app::Document*>(opened.value()));
}

bool MainWindow::saveDocument(bool forceDialog) {
    app::Document* doc = activeDocument();
    if (doc == nullptr) {
        return false;
    }
    std::string path = doc->fullPath();
    if (forceDialog || path.empty()) {
        const QString chosen =
            QFileDialog::getSaveFileName(this, "저장", QString(), "OpenRaster (*.ora)");
        if (chosen.isEmpty()) {
            return false;
        }
        path = chosen.toStdString();
        if (path.size() < 4 || path.substr(path.size() - 4) != ".ora") {
            path += ".ora";
        }
    }
    const Result<void> saved = doc->saveAs(path, "ora");
    if (!saved.ok()) {
        QMessageBox::warning(this, "저장", QString::fromStdString(saved.message()));
        return false;
    }
    refreshTitle();
    statusBar()->showMessage("저장했다: " + QString::fromStdString(path), 3000);
    return true;
}

bool MainWindow::confirmDiscard() {
    app::Document* doc = activeDocument();
    if (doc == nullptr || doc->isSaved()) {
        return true;
    }
    const auto r = QMessageBox::question(this, "저장하지 않은 변경",
                                         "저장하지 않은 변경이 있다. 저장할까?",
                                         QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
    if (r == QMessageBox::Cancel) {
        return false;
    }
    if (r == QMessageBox::Save) {
        return saveDocument(false);
    }
    return true;
}

void MainWindow::closeEvent(QCloseEvent* e) {
    if (!confirmDiscard()) {
        e->ignore();
        return;
    }
    e->accept();
}

// ── 편집 ─────────────────────────────────────────────────────────────────

app::LiveStrokeConfig MainWindow::strokeConfig() const {
    app::LiveStrokeConfig cfg;
    const int i = brushCombo_->currentIndex();
    if (i >= 0 && static_cast<usize>(i) < brushes_.size()) {
        cfg.preset = brushes_[static_cast<usize>(i)];
        cfg.brushId = static_cast<BrushId>(i + 1); // agent-api 의 내장 붓 id 와 같은 순서
    }
    cfg.preset.tip.diameter = static_cast<f32>(sizeSpin_->value());
    cfg.color = Color8::rgba(static_cast<u8>(color_.red()), static_cast<u8>(color_.green()),
                             static_cast<u8>(color_.blue()), 255);
    cfg.eraser = eraserAction_->isChecked();
    switch (smoothingCombo_->currentIndex()) {
    case 1: cfg.smoothing = stroke::SmoothingMode::Light; break;
    case 2: cfg.smoothing = stroke::SmoothingMode::Medium; break;
    case 3: cfg.smoothing = stroke::SmoothingMode::Strong; break;
    default: cfg.smoothing = stroke::SmoothingMode::Off; break;
    }
    return cfg;
}

void MainWindow::undo() {
    app::Document* doc = activeDocument();
    if (doc == nullptr || canvas_->strokeActive()) {
        return;
    }
    const Result<void> r = doc->undo();
    if (!r.ok()) {
        statusBar()->showMessage(QString::fromStdString(r.message()), 3000);
    }
    canvas_->invalidateCanvas();
    refreshLayerList();
    refreshTitle();
}

void MainWindow::redo() {
    app::Document* doc = activeDocument();
    if (doc == nullptr || canvas_->strokeActive()) {
        return;
    }
    const Result<void> r = doc->redo();
    if (!r.ok()) {
        statusBar()->showMessage(QString::fromStdString(r.message()), 3000);
    }
    canvas_->invalidateCanvas();
    refreshLayerList();
    refreshTitle();
}

void MainWindow::pickColor() {
    const QColor c = QColorDialog::getColor(color_, this, "색");
    if (!c.isValid()) {
        return;
    }
    color_ = c;
    const bool dark = c.lightness() < 128;
    colorButton_->setStyleSheet(QString("QToolButton { background: %1; color: %2; min-width: 48px; }")
                                    .arg(c.name(), dark ? "white" : "black"));
}

// ── 레이어 ───────────────────────────────────────────────────────────────

void MainWindow::refreshLayerList() {
    layerListBusy_ = true;
    layerList_->clear();
    app::Document* doc = activeDocument();
    if (doc != nullptr) {
        // 인덱스 0 이 가장 아래다(core 규약). 목록은 위가 위로 보이게 뒤집는다.
        const std::vector<LayerPtr>& roots = doc->layers().roots();
        const LayerId active = doc->layers().activeLayer();
        int activeRow = -1;
        for (auto it = roots.rbegin(); it != roots.rend(); ++it) {
            const Layer& l = **it;
            auto* item = new QListWidgetItem(layerLabel(l), layerList_);
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(l.visible() ? Qt::Checked : Qt::Unchecked);
            item->setData(Qt::UserRole, QVariant::fromValue<qulonglong>(l.id()));
            if (l.id() == active) {
                activeRow = layerList_->count() - 1;
            }
        }
        if (activeRow >= 0) {
            layerList_->setCurrentRow(activeRow);
        }
    }
    layerListBusy_ = false;
}

void MainWindow::onLayerRowChanged(int row) {
    if (layerListBusy_ || row < 0) {
        return;
    }
    app::Document* doc = activeDocument();
    if (doc == nullptr) {
        return;
    }
    const auto id = static_cast<LayerId>(layerList_->item(row)->data(Qt::UserRole).toULongLong());
    (void)doc->layers().setActiveLayer(id);
}

void MainWindow::onLayerItemChanged(QListWidgetItem* item) {
    if (layerListBusy_ || item == nullptr) {
        return;
    }
    app::Document* doc = activeDocument();
    if (doc == nullptr) {
        return;
    }
    const auto id = static_cast<LayerId>(item->data(Qt::UserRole).toULongLong());
    if (const LayerPtr l = doc->layers().find(id)) {
        l->setVisible(item->checkState() == Qt::Checked);
        doc->markDirty();
        canvas_->invalidateCanvas();
        refreshTitle();
    }
}

void MainWindow::addLayer() {
    app::Document* doc = activeDocument();
    if (doc == nullptr || canvas_->strokeActive()) {
        return;
    }
    const std::string name = "레이어 " + std::to_string(doc->layers().roots().size() + 1);
    const Result<LayerPtr> made = doc->layers().addRaster(name);
    if (!made.ok()) {
        statusBar()->showMessage(QString::fromStdString(made.message()), 3000);
        return;
    }
    (void)doc->layers().setActiveLayer(made.value()->id());
    doc->markDirty();
    refreshLayerList();
    refreshTitle();
}

void MainWindow::removeLayer() {
    app::Document* doc = activeDocument();
    if (doc == nullptr || canvas_->strokeActive()) {
        return;
    }
    if (doc->layers().roots().size() <= 1) {
        statusBar()->showMessage("마지막 레이어는 지울 수 없다", 3000);
        return;
    }
    const LayerId id = doc->layers().activeLayer();
    const Result<void> r = doc->layers().remove(id);
    if (!r.ok()) {
        statusBar()->showMessage(QString::fromStdString(r.message()), 3000);
        return;
    }
    if (!doc->layers().roots().empty()) {
        (void)doc->layers().setActiveLayer(doc->layers().roots().back()->id());
    }
    doc->markDirty();
    canvas_->invalidateCanvas();
    refreshLayerList();
    refreshTitle();
}

// ── 상태 ─────────────────────────────────────────────────────────────────

void MainWindow::refreshStatus() {
    const app::Document* doc = activeDocument();
    if (doc != nullptr && doc->recorder() != nullptr) {
        statusRecord_->setText(doc->recordingBroken() ? "기록: 🔴 고장" : "기록: 저널 " + journalPath_);
    } else {
        statusRecord_->setText("기록: 없음");
    }
    statusStrokes_->setText(QString("획 %1 · 기록됨 %2 · 롤백 %3")
                                .arg(strokes_).arg(recordedStrokes_).arg(rolledBack_));

    const LatencyStats& lat = canvas_->latency();
    if (lat.samples == 0) {
        statusLatency_->setText("펜→화면: —");
    } else {
        statusLatency_->setText(QString("펜→화면 %1 ms (평균 %2 · 최대 %3 · >16ms %4/%5)")
                                    .arg(lat.lastMs, 0, 'f', 1)
                                    .arg(lat.avgMs, 0, 'f', 1)
                                    .arg(lat.maxMs, 0, 'f', 1)
                                    .arg(lat.over16)
                                    .arg(lat.samples));
    }
    const mari::win::ViewState& v = canvas_->viewState();
    statusView_->setText(QString("%1% · %2°").arg(v.zoom * 100.0, 0, 'f', 0).arg(v.rotationDeg, 0, 'f', 0));
    if (undoAction_ != nullptr && doc != nullptr) {
        undoAction_->setEnabled(activeDocument()->undoStack().canUndo());
        redoAction_->setEnabled(activeDocument()->undoStack().canRedo());
    }
}

void MainWindow::refreshTitle() {
    const app::Document* doc = activeDocument();
    QString title = "Mari Paint";
    if (doc != nullptr) {
        const std::string p = doc->fullPath();
        title = (p.empty() ? QString("제목 없음") : QString::fromStdString(p)) +
                (doc->isSaved() ? "" : " *") + " — Mari Paint";
    }
    setWindowTitle(title);
    refreshStatus();
}

} // namespace mari::ui
