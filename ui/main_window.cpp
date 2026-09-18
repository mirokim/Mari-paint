// Mari Paint — 최소 UI 구현 (ui/main_window.hpp)
#include "main_window.hpp"

#include "canvas_widget.hpp"
#include "color_panel.hpp"
#include "icons.hpp"
#include "layer_panel.hpp"
#include "navigator.hpp"
#include "popup_palette.hpp"
#include "tablet_dialog.hpp"

#include <mari/app/layer_commands.hpp>

#include <mari/brush/builtin.hpp>
#include <mari/win/input/pointer_input.hpp>

#include <QAction>
#include <QActionGroup>
#include <QCloseEvent>
#include <QComboBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QFileDialog>
#include <QInputDialog>
#include <QKeySequence>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QSettings>
#include <QSlider>
#include <QSpinBox>
#include <QStatusBar>
#include <QTimer>
#include <QToolBar>

#include <cmath>
#include <cstdio>

#include <windows.h>

namespace {
// 진단: 프로세스 힙이 온전한지 그 자리에서 확인한다(손상이면 죽지 않고 false 를 준다).
void heapCheck(const char* where) {
    if (!qEnvironmentVariableIsSet("MARI_GUI_TRACE")) return;
    const BOOL ok = ::HeapValidate(::GetProcessHeap(), 0, nullptr);
    std::fprintf(stderr, "[heap] %-32s %s\n", where, ok ? "ok" : "CORRUPT");
    std::fflush(stderr);
}
} // namespace

namespace mari::ui {

namespace {

// 붓 크기 슬라이더: 0..1000 → 1..500px 지수 매핑. 작은 값에 정밀도가 몰린다(Krita·SAI 관례).
constexpr f64 kSizeMin = 1.0;
constexpr f64 kSizeMax = 500.0;
constexpr int kSliderMax = 1000;

int sizeToSlider(f64 px) {
    const f64 t = std::log(std::clamp(px, kSizeMin, kSizeMax) / kSizeMin) / std::log(kSizeMax / kSizeMin);
    return static_cast<int>(std::lround(t * kSliderMax));
}
f64 sliderToSize(int v) {
    const f64 t = static_cast<f64>(std::clamp(v, 0, kSliderMax)) / kSliderMax;
    return kSizeMin * std::pow(kSizeMax / kSizeMin, t);
}
/// `[` `]` 스텝: 구간별(PS·CSP 관례). 선형 1px 스텝은 큰 붓에서 쓸모없다.
f64 sizeStep(f64 px) {
    if (px < 10.0) return 1.0;
    if (px < 50.0) return 5.0;
    if (px < 100.0) return 10.0;
    if (px < 300.0) return 25.0;
    return 50.0;
}

} // namespace

MainWindow::MainWindow(app::Application& app, QString journalPath, QWidget* parent)
    : QMainWindow(parent), app_(app), journalPath_(std::move(journalPath)) {
    heapCheck("MainWindow ctor start");
    brushes_ = brush::builtinPresets();

    canvas_ = new CanvasWidget(this);
    setCentralWidget(canvas_);
    canvas_->setStrokeConfigProvider([this] { return strokeConfig(); });

    buildMenus();
    buildToolbars();
    buildDocks();
    buildStatusBar();
    heapCheck("after UI build");

    thumbTimer_ = new QTimer(this);
    thumbTimer_->setSingleShot(true);
    thumbTimer_->setInterval(200);
    connect(thumbTimer_, &QTimer::timeout, this, [this] {
        if (app::Document* doc = activeDocument()) {
            layerPanel_->refreshThumbnail(doc->layers().activeLayer());
        }
        navigator_->refreshImage();
    });

    connect(canvas_, &CanvasWidget::strokeFinished, this, [this](const app::LiveStrokeOutcome& o) {
        ++strokes_;
        if (o.recorded) ++recordedStrokes_;
        if (o.rolledBack) {
            ++rolledBack_;
            statusBar()->showMessage("[기록 실패] 저널에 기록하지 못해 이 획을 되돌렸다 (docs/06 결정 ④)", 8000);
        } else if (!o.undoComplete) {
            statusBar()->showMessage("[경고] 실행취소 범위가 칠한 범위를 다 덮지 못했다", 5000);
        } else if (!o.engineNote.empty()) {
            statusBar()->showMessage(QString::fromStdString(o.engineNote), 5000);
        }
        if (canvas_->tool() != Tool::Eraser) {
            colorPanel_->noteUsed(colorPanel_->foreground());
        }
        // 썸네일은 레이어 전체를 내보내 축소하므로 획의 마지막 프레임과 같은 프레임에 두지 않는다
        // (펜→화면 지연에 그대로 얹힌다, 실측 +40ms). 잠깐 미루고 연속 획은 한 번으로 합친다.
        thumbTimer_->start();
        refreshStatus();
        refreshTitle();
    });
    connect(canvas_, &CanvasWidget::strokeRefused, this, [this](const QString& why) {
        statusBar()->showMessage("획을 시작하지 못했다: " + why, 6000);
    });
    connect(canvas_, &CanvasWidget::latencyUpdated, this, &MainWindow::refreshStatus);
    connect(canvas_, &CanvasWidget::viewChanged, this, &MainWindow::refreshStatus);
    connect(canvas_, &CanvasWidget::colorPicked, this, [this](const QColor& c) { colorPanel_->setForeground(c); });
    connect(canvas_, &CanvasWidget::paletteRequested, this, &MainWindow::showPalette);
    connect(canvas_, &CanvasWidget::brushSizeGesture, this, [this](f64 d) { setBrushSize(d); });
    connect(canvas_, &CanvasWidget::toolChanged, this, [this](Tool t) {
        for (QAction* a : toolGroup_->actions()) {
            if (a->data().toInt() == static_cast<int>(t)) a->setChecked(true);
        }
        floodOptions_->setVisible(t == Tool::Fill || t == Tool::SelectWand);
        refreshStatus();
    });
    connect(canvas_, &CanvasWidget::regionFilled, this, [this] {
        thumbTimer_->start();
        refreshTitle();
    });
    connect(canvas_, &CanvasWidget::selectionChanged, this, &MainWindow::refreshStatus);

    attachDocument(activeDocument());
    resize(1400, 900);
    {
        QSettings settings;
        const QByteArray geo = settings.value("window/geometry").toByteArray();
        const QByteArray state = settings.value("window/state").toByteArray();
        if (!geo.isEmpty()) restoreGeometry(geo);
        if (!state.isEmpty()) restoreState(state);
    }
    refreshTitle();
    refreshStatus();

    if (qEnvironmentVariableIsSet("MARI_GUI_TRACE")) {
        QTimer::singleShot(1500, this, [this] {
            const auto g = [](const char* n, const QWidget* w) {
                const QRect r = w->geometry();
                std::fprintf(stderr, "[mari-gui] %s geom=%d,%d %dx%d visible=%d\n", n, r.x(), r.y(), r.width(),
                             r.height(), w->isVisible() ? 1 : 0);
            };
            std::fprintf(stderr, "[mari-gui] dpr=%.2f\n", devicePixelRatioF());
            g("window", this);
            g("canvas", canvas_);
            g("statusbar", statusBar());
            g("colordock", colorDock_);
            g("layerdock", layerDock_);
            std::fflush(stderr);
        });
    }
}

MainWindow::~MainWindow() = default;

// ── 구성 ─────────────────────────────────────────────────────────────────

void MainWindow::buildMenus() {
    QMenu* file = menuBar()->addMenu("파일(&F)");
    file->addAction(themedIcon("file-plus"), "새 문서(&N)...", QKeySequence::New, this, &MainWindow::newDocument);
    file->addAction(themedIcon("folder-open"), "열기(&O)...", QKeySequence::Open, this, &MainWindow::openDocument);
    file->addAction(themedIcon("device-floppy"), "저장(&S)", QKeySequence::Save, this, [this] { saveDocument(false); });
    file->addAction("다른 이름으로 저장(&A)...", QKeySequence::SaveAs, this, [this] { saveDocument(true); });
    file->addSeparator();
    file->addAction("종료(&Q)", QKeySequence::Quit, this, &QWidget::close);

    QMenu* edit = menuBar()->addMenu("편집(&E)");
    undoAction_ = edit->addAction(themedIcon("arrow-back-up", 20), "실행 취소(&U)", QKeySequence::Undo, this, &MainWindow::undo);
    redoAction_ = edit->addAction(themedIcon("arrow-forward-up", 20), "다시 실행(&R)", QKeySequence::Redo, this, &MainWindow::redo);
    redoAction_->setShortcuts({QKeySequence::Redo, QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Z)});
    edit->addSeparator();
    edit->addAction("붓 크게  ]", QKeySequence(Qt::Key_BracketRight), this, [this] { stepBrushSize(+1); });
    edit->addAction("붓 작게  [", QKeySequence(Qt::Key_BracketLeft), this, [this] { stepBrushSize(-1); });
    edit->addAction("불투명도 +  Shift+]", QKeySequence(Qt::SHIFT | Qt::Key_BracketRight), this,
                    [this] { stepOpacity(+1); });
    edit->addAction("불투명도 −  Shift+[", QKeySequence(Qt::SHIFT | Qt::Key_BracketLeft), this,
                    [this] { stepOpacity(-1); });
    edit->addSeparator();
    edit->addAction("전경↔배경 교환  X", QKeySequence(Qt::Key_X), this, [this] { colorPanel_->swap(); });
    edit->addAction("기본 색(검정/흰색)  D", QKeySequence(Qt::Key_D), this, [this] { colorPanel_->resetDefaults(); });

    QMenu* select = menuBar()->addMenu("선택(&S)");
    select->addAction("전체 선택", QKeySequence::SelectAll, this, [this] { canvas_->selectAll(); });
    select->addAction("선택 해제", QKeySequence(Qt::CTRL | Qt::Key_D), this, [this] { canvas_->deselect(); });
    select->addAction("선택 반전", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_I), this,
                      [this] { canvas_->invertSelection(); });
    select->addSeparator();
    select->addAction("수식키: Shift 더하기 · Alt 빼기 · Shift+Alt 교집합")->setEnabled(false);

    QMenu* layer = menuBar()->addMenu("레이어(&L)");
    layer->addAction("새 레이어", QKeySequence(Qt::Key_Insert), this, [this] { layerPanel_->addLayer(); });
    layer->addAction(themedIcon("copy", 20), "레이어 복제", QKeySequence(Qt::CTRL | Qt::Key_J), this,
                     [this] { layerPanel_->duplicateLayer(); });
    layer->addAction(themedIcon("arrow-merge", 20), "아래와 병합", QKeySequence(Qt::CTRL | Qt::Key_E), this,
                     &MainWindow::mergeDown);
    layer->addAction(themedIcon("trash", 20), "레이어 삭제", this, [this] { layerPanel_->removeLayer(); });
    layer->addAction("위 레이어 선택", QKeySequence(Qt::Key_PageUp), this, [this] {
        if (app::Document* d = activeDocument()) {
            const auto& roots = d->layers().roots();
            for (usize i = 0; i + 1 < roots.size(); ++i)
                if (roots[i]->id() == d->layers().activeLayer()) {
                    (void)d->layers().setActiveLayer(roots[i + 1]->id());
                    layerPanel_->refresh();
                    break;
                }
        }
    });
    layer->addAction("아래 레이어 선택", QKeySequence(Qt::Key_PageDown), this, [this] {
        if (app::Document* d = activeDocument()) {
            const auto& roots = d->layers().roots();
            for (usize i = 1; i < roots.size(); ++i)
                if (roots[i]->id() == d->layers().activeLayer()) {
                    (void)d->layers().setActiveLayer(roots[i - 1]->id());
                    layerPanel_->refresh();
                    break;
                }
        }
    });

    QMenu* view = menuBar()->addMenu("보기(&V)");
    viewActions_.zoomIn = view->addAction(themedIcon("zoom-in", 20), "확대", QKeySequence::ZoomIn, this,
                                          [this] { canvas_->zoomBy(1.25, QRectF(canvas_->rect()).center()); });
    viewActions_.zoomOut = view->addAction(themedIcon("zoom-out", 20), "축소", QKeySequence::ZoomOut, this,
                                           [this] { canvas_->zoomBy(0.8, QRectF(canvas_->rect()).center()); });
    viewActions_.zoomReset = view->addAction(themedIcon("zoom-reset", 20), "100%", QKeySequence(Qt::CTRL | Qt::Key_1),
                                             this, [this] { canvas_->resetView(); });
    viewActions_.fit = view->addAction(themedIcon("arrows-maximize", 20), "창에 맞춤", QKeySequence(Qt::CTRL | Qt::Key_0),
                                       this, [this] { canvas_->fitToView(); });
    view->addSeparator();
    viewActions_.rotL = view->addAction(themedIcon("rotate", 20), "왼쪽으로 회전", QKeySequence(Qt::CTRL | Qt::Key_BracketLeft),
                                        this, [this] { canvas_->rotateBy(-15.0); });
    viewActions_.rotR = view->addAction(themedIcon("rotate-clockwise", 20), "오른쪽으로 회전",
                                        QKeySequence(Qt::CTRL | Qt::Key_BracketRight), this, [this] { canvas_->rotateBy(15.0); });
    view->addAction("회전 초기화", QKeySequence(Qt::Key_5), this, [this] { canvas_->resetRotation(); });
    viewActions_.mirror = view->addAction(themedIcon("flip-horizontal", 20), "미러 보기", QKeySequence(Qt::Key_M), this,
                                          [this] { canvas_->toggleMirror(); });
    view->addSeparator();
    viewActions_.panels = view->addAction(themedIcon("layout-sidebar-right-collapse", 20), "패널 숨김/표시",
                                          QKeySequence(Qt::Key_Tab), this, &MainWindow::togglePanels);
    view->addAction("캔버스 전용 모드", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F), this,
                    &MainWindow::toggleCanvasOnly);
    view->addSeparator();
    view->addAction(themedIcon("settings", 20), "태블릿 — 압력 곡선 · 테스터...", this, &MainWindow::showTabletDialog);
    debugStatusAction_ = view->addAction("진단 상태 표시(지연·저널)");
    debugStatusAction_->setCheckable(true);
    debugStatusAction_->setChecked(qEnvironmentVariableIsSet("MARI_GUI_TRACE"));
    connect(debugStatusAction_, &QAction::toggled, this, [this](bool) { refreshStatus(); });
    view->addSeparator();
    view->addAction("Space+드래그 팬 · Ctrl+Space 줌 · Shift+Space 회전 · Alt 스포이드")->setEnabled(false);
}

void MainWindow::buildToolbars() {
    // 왼쪽 세로 도구상자 — B/E/I/H
    toolsBar_ = new QToolBar("도구", this);
    toolsBar_->setObjectName("tools");
    toolsBar_->setMovable(false);
    toolsBar_->setOrientation(Qt::Vertical);
    toolsBar_->setToolButtonStyle(Qt::ToolButtonIconOnly);
    toolsBar_->setIconSize(QSize(22, 22));
    addToolBar(Qt::LeftToolBarArea, toolsBar_);
    toolGroup_ = new QActionGroup(this);
    toolGroup_->setExclusive(true);
    const auto addTool = [&](const char* icon, const char* text, const char* tip, Qt::Key key, Tool t) {
        QAction* a = toolsBar_->addAction(themedIcon(icon, 22), text);
        a->setCheckable(true);
        a->setToolTip(tip);
        a->setShortcut(QKeySequence(key));
        a->setData(static_cast<int>(t));
        toolGroup_->addAction(a);
        connect(a, &QAction::triggered, this, [this, t] { canvas_->setTool(t); });
        return a;
    };
    addTool("brush", "붓", "붓 (B)", Qt::Key_B, Tool::Brush)->setChecked(true);
    addTool("eraser", "지우개", "지우개 (E)", Qt::Key_E, Tool::Eraser);
    addTool("color-picker", "스포이드", "스포이드 (I · Alt)", Qt::Key_I, Tool::Eyedropper);
    addTool("hand-stop", "손", "손 (H · Space)", Qt::Key_H, Tool::Hand);
    toolsBar_->addSeparator();
    addTool("bucket-droplet", "채우기", "페인트통 (G) — 클릭한 곳과 이어진 영역을 전경색으로", Qt::Key_G, Tool::Fill);
    toolsBar_->addSeparator();
    addTool("square-dashed", "사각 선택", "사각형 선택 (M 키는 미러라 R) — Shift 더하기 · Alt 빼기 · Shift+Alt 교집합", Qt::Key_R, Tool::SelectRect);
    addTool("circle-dashed", "타원 선택", "타원 선택 (O)", Qt::Key_O, Tool::SelectEllipse);
    addTool("lasso", "올가미", "올가미 선택 (L)", Qt::Key_L, Tool::SelectLasso);
    addTool("wand", "마술봉", "마술봉 (W) — 이어진 같은 색 영역", Qt::Key_W, Tool::SelectWand);

    // 상단 옵션 툴바 — 붓 프리셋 · 크기 · 불투명도 · 보정 · 실행취소
    optionsBar_ = addToolBar("옵션");
    optionsBar_->setObjectName("options");
    optionsBar_->setMovable(false);
    optionsBar_->setToolButtonStyle(Qt::ToolButtonIconOnly);
    optionsBar_->setIconSize(QSize(20, 20));

    optionsBar_->addWidget(new QLabel("붓"));
    brushCombo_ = new QComboBox(optionsBar_);
    for (const brush::MariBrushPreset& p : brushes_) {
        brushCombo_->addItem(QString::fromStdString(p.name));
    }
    optionsBar_->addWidget(brushCombo_);
    connect(brushCombo_, &QComboBox::currentIndexChanged, this, [this](int i) {
        if (i >= 0 && static_cast<usize>(i) < brushes_.size()) {
            setBrushSize(brushes_[static_cast<usize>(i)].tip.diameter);
            opacitySlider_->setValue(static_cast<int>(brushes_[static_cast<usize>(i)].opacity * 100.0f + 0.5f));
        }
    });

    optionsBar_->addSeparator();
    optionsBar_->addWidget(new QLabel("크기"));
    sizeSlider_ = new QSlider(Qt::Horizontal, optionsBar_);
    sizeSlider_->setRange(0, kSliderMax);
    sizeSlider_->setFixedWidth(160);
    optionsBar_->addWidget(sizeSlider_);
    sizeSpin_ = new QDoubleSpinBox(optionsBar_);
    sizeSpin_->setRange(kSizeMin, kSizeMax);
    sizeSpin_->setDecimals(1);
    sizeSpin_->setSuffix(" px");
    sizeSpin_->setFixedWidth(84);
    sizeSpin_->setButtonSymbols(QAbstractSpinBox::NoButtons);
    optionsBar_->addWidget(sizeSpin_);
    connect(sizeSlider_, &QSlider::valueChanged, this, [this](int v) {
        if (!syncingSize_) setBrushSize(sliderToSize(v));
    });
    connect(sizeSpin_, &QDoubleSpinBox::valueChanged, this, [this](double v) {
        if (!syncingSize_) setBrushSize(v);
    });

    optionsBar_->addSeparator();
    optionsBar_->addWidget(new QLabel("불투명도"));
    opacitySlider_ = new QSlider(Qt::Horizontal, optionsBar_);
    opacitySlider_->setRange(1, 100);
    opacitySlider_->setValue(100);
    opacitySlider_->setFixedWidth(120);
    opacitySlider_->setToolTip("불투명도 (Shift+[ · Shift+])");
    optionsBar_->addWidget(opacitySlider_);
    opacitySpin_ = new QSpinBox(optionsBar_);
    opacitySpin_->setRange(1, 100);
    opacitySpin_->setSuffix("%");
    opacitySpin_->setFixedWidth(56);
    opacitySpin_->setButtonSymbols(QAbstractSpinBox::NoButtons);
    optionsBar_->addWidget(opacitySpin_);
    connect(opacitySlider_, &QSlider::valueChanged, this, [this](int v) {
        if (opacitySpin_->value() != v) opacitySpin_->setValue(v);
        refreshStatus();
    });
    connect(opacitySpin_, &QSpinBox::valueChanged, this, [this](int v) { opacitySlider_->setValue(v); });

    optionsBar_->addSeparator();
    optionsBar_->addWidget(new QLabel("보정"));
    smoothingCombo_ = new QComboBox(optionsBar_);
    smoothingCombo_->addItems({"끔", "약함", "보통", "강함"});
    optionsBar_->addWidget(smoothingCombo_);

    // 마술봉·페인트통 옵션(해당 도구일 때만 보인다)
    floodOptions_ = new QWidget(optionsBar_);
    {
        auto* row = new QHBoxLayout(floodOptions_);
        row->setContentsMargins(6, 0, 0, 0);
        row->setSpacing(6);
        row->addWidget(new QLabel("허용 오차", floodOptions_));
        toleranceSpin_ = new QSpinBox(floodOptions_);
        toleranceSpin_->setRange(0, 255);
        toleranceSpin_->setValue(32);
        toleranceSpin_->setFixedWidth(56);
        row->addWidget(toleranceSpin_);
        row->addWidget(new QLabel("틈 닫기", floodOptions_));
        gapSpin_ = new QSpinBox(floodOptions_);
        gapSpin_->setRange(0, 16);
        gapSpin_->setSuffix(" px");
        gapSpin_->setFixedWidth(64);
        row->addWidget(gapSpin_);
        const auto push = [this] { canvas_->setFloodOptions(toleranceSpin_->value(), gapSpin_->value()); };
        connect(toleranceSpin_, &QSpinBox::valueChanged, this, [push](int) { push(); });
        connect(gapSpin_, &QSpinBox::valueChanged, this, [push](int) { push(); });
        push();
    }
    optionsBar_->addWidget(floodOptions_);
    floodOptions_->setVisible(false);

    optionsBar_->addSeparator();
    optionsBar_->addAction(undoAction_);
    optionsBar_->addAction(redoAction_);
    optionsBar_->addSeparator();
    optionsBar_->addAction(viewActions_.zoomOut);
    optionsBar_->addAction(viewActions_.zoomReset);
    optionsBar_->addAction(viewActions_.zoomIn);
    optionsBar_->addAction(viewActions_.fit);
    optionsBar_->addSeparator();
    optionsBar_->addAction(viewActions_.rotL);
    optionsBar_->addAction(viewActions_.rotR);
    optionsBar_->addAction(viewActions_.mirror);
    auto* spacer = new QWidget(optionsBar_);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    optionsBar_->addWidget(spacer);
    optionsBar_->addAction(viewActions_.panels);

    setBrushSize(brushes_.empty() ? 10.0 : brushes_.front().tip.diameter);
    if (!brushes_.empty()) {
        opacitySlider_->setValue(static_cast<int>(brushes_.front().opacity * 100.0f + 0.5f));
    }
}

void MainWindow::buildDocks() {
    colorDock_ = new QDockWidget("색", this);
    colorDock_->setFeatures(QDockWidget::DockWidgetMovable);
    colorPanel_ = new ColorPanel(colorDock_);
    colorDock_->setWidget(colorPanel_);
    addDockWidget(Qt::RightDockWidgetArea, colorDock_);

    layerDock_ = new QDockWidget("레이어", this);
    layerDock_->setFeatures(QDockWidget::DockWidgetMovable);
    layerPanel_ = new LayerPanel(layerDock_);
    layerDock_->setWidget(layerPanel_);
    addDockWidget(Qt::RightDockWidgetArea, layerDock_);
    navDock_ = new QDockWidget("내비게이터", this);
    navDock_->setObjectName("navDock");
    navDock_->setFeatures(QDockWidget::DockWidgetMovable);
    navigator_ = new Navigator(canvas_, navDock_);
    navDock_->setWidget(navigator_);
    addDockWidget(Qt::RightDockWidgetArea, navDock_);
    colorDock_->setObjectName("colorDock");
    layerDock_->setObjectName("layerDock");
    resizeDocks({navDock_, colorDock_, layerDock_}, {150, 330, 400}, Qt::Vertical);
    resizeDocks({colorDock_}, {280}, Qt::Horizontal);

    palette_ = new PopupPalette(this);
    connect(palette_, &PopupPalette::brushChosen, this, [this](int i) { brushCombo_->setCurrentIndex(i); });
    connect(palette_, &PopupPalette::colorChosen, this, [this](const QColor& c) { colorPanel_->setForeground(c); });

    connect(layerPanel_, &LayerPanel::layersChanged, this, [this] {
        canvas_->invalidateCanvas();
        thumbTimer_->start();
        refreshTitle();
    });
    connect(layerPanel_, &LayerPanel::mergeDownRequested, this, &MainWindow::mergeDown);
    connect(layerPanel_, &LayerPanel::activeLayerChanged, this, [this](LayerId) { refreshStatus(); });
    connect(colorPanel_, &ColorPanel::foregroundChanged, this, [this](const QColor&) { refreshStatus(); });
}

void MainWindow::buildStatusBar() {
    statusToolIcon_ = new QLabel(this);
    statusMain_ = new QLabel(this);
    statusDebug_ = new QLabel(this);
    statusDebug_->setMaximumWidth(640);
    statusView_ = new QLabel(this);
    statusBar()->addWidget(statusToolIcon_);
    statusBar()->addWidget(statusMain_, 1);
    statusBar()->addPermanentWidget(statusDebug_);
    statusBar()->addPermanentWidget(statusView_);
    statusBar()->setSizeGripEnabled(false);
}

// ── 문서 ─────────────────────────────────────────────────────────────────

app::Document* MainWindow::activeDocument() const {
    // Application 은 Document 만 들고 있다(unique_ptr<Document>). 브리지에서 내려 받는다.
    return static_cast<app::Document*>(app_.activeDocument());
}

void MainWindow::attachDocument(app::Document* doc) {
    canvas_->setDocument(doc);
    layerPanel_->setDocument(doc);
    if (navigator_ != nullptr) navigator_->refreshImage();
    refreshTitle();
    refreshStatus();
}

void MainWindow::newDocument() {
    if (!confirmDiscard()) return;
    bool ok = false;
    const int w = QInputDialog::getInt(this, "새 문서", "너비(px)", 1920, 1, 16384, 1, &ok);
    if (!ok) return;
    const int h = QInputDialog::getInt(this, "새 문서", "높이(px)", 1080, 1, 16384, 1, &ok);
    if (!ok) return;
    if (app::Document* old = activeDocument()) {
        canvas_->setDocument(nullptr);
        layerPanel_->setDocument(nullptr);
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
    if (!confirmDiscard()) return;
    const QString path = QFileDialog::getOpenFileName(this, "열기", QString(), "OpenRaster (*.ora)");
    if (path.isEmpty()) return;
    if (app::Document* old = activeDocument()) {
        canvas_->setDocument(nullptr);
        layerPanel_->setDocument(nullptr);
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
    if (doc == nullptr) return false;
    std::string path = doc->fullPath();
    if (forceDialog || path.empty()) {
        const QString chosen = QFileDialog::getSaveFileName(this, "저장", QString(), "OpenRaster (*.ora)");
        if (chosen.isEmpty()) return false;
        path = chosen.toStdString();
        if (path.size() < 4 || path.substr(path.size() - 4) != ".ora") path += ".ora";
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
    if (doc == nullptr || doc->isSaved()) return true;
    const auto r = QMessageBox::question(this, "저장하지 않은 변경", "저장하지 않은 변경이 있다. 저장할까?",
                                         QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
    if (r == QMessageBox::Cancel) return false;
    if (r == QMessageBox::Save) return saveDocument(false);
    return true;
}

void MainWindow::closeEvent(QCloseEvent* e) {
    if (!confirmDiscard()) {
        e->ignore();
        return;
    }
    if (canvasOnly_) toggleCanvasOnly();
    if (panelsHidden_) togglePanels();
    QSettings settings;
    settings.setValue("window/geometry", saveGeometry());
    settings.setValue("window/state", saveState());
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
    cfg.preset.opacity = static_cast<f32>(opacitySlider_->value()) / 100.0f;
    const QColor c = colorPanel_->foreground();
    cfg.color = Color8::rgba(static_cast<u8>(c.red()), static_cast<u8>(c.green()), static_cast<u8>(c.blue()), 255);
    cfg.eraser = canvas_->tool() == Tool::Eraser;
    switch (smoothingCombo_->currentIndex()) {
    case 1: cfg.smoothing = stroke::SmoothingMode::Light; break;
    case 2: cfg.smoothing = stroke::SmoothingMode::Medium; break;
    case 3: cfg.smoothing = stroke::SmoothingMode::Strong; break;
    default: cfg.smoothing = stroke::SmoothingMode::Off; break;
    }
    return cfg;
}

void MainWindow::setBrushSize(f64 px) {
    px = std::clamp(px, kSizeMin, kSizeMax);
    syncingSize_ = true;
    sizeSlider_->setValue(sizeToSlider(px));
    sizeSpin_->setValue(px);
    syncingSize_ = false;
    canvas_->setBrushDiameter(px);
    refreshStatus();
}

void MainWindow::stepBrushSize(int direction) {
    const f64 cur = sizeSpin_->value();
    // 경계 아래로 내려갈 때는 아래 구간의 스텝을 쓴다(10 → 9, 50 → 45 …).
    const f64 step = direction > 0 ? sizeStep(cur) : sizeStep(cur - 0.5);
    setBrushSize(cur + direction * step);
}

void MainWindow::stepOpacity(int direction) {
    opacitySlider_->setValue(std::clamp(opacitySlider_->value() + direction * 10, 1, 100));
}

void MainWindow::undo() {
    app::Document* doc = activeDocument();
    if (doc == nullptr || canvas_->strokeActive()) return;
    const Result<void> r = doc->undo();
    if (!r.ok()) statusBar()->showMessage(QString::fromStdString(r.message()), 3000);
    canvas_->invalidateCanvas();
    layerPanel_->refresh();
    thumbTimer_->start();
    refreshTitle();
}

void MainWindow::redo() {
    app::Document* doc = activeDocument();
    if (doc == nullptr || canvas_->strokeActive()) return;
    const Result<void> r = doc->redo();
    if (!r.ok()) statusBar()->showMessage(QString::fromStdString(r.message()), 3000);
    canvas_->invalidateCanvas();
    layerPanel_->refresh();
    refreshTitle();
}

void MainWindow::toggleCanvasOnly() {
    canvasOnly_ = !canvasOnly_;
    if (canvasOnly_) {
        savedLayoutState_ = saveState();
        menuBar()->hide();
        statusBar()->hide();
        if (!panelsHidden_) togglePanels();
        showFullScreen();
    } else {
        showNormal();
        menuBar()->show();
        statusBar()->show();
        if (panelsHidden_) togglePanels();
        if (!savedLayoutState_.isEmpty()) restoreState(savedLayoutState_);
    }
}

void MainWindow::showTabletDialog() {
    auto* dlg = new TabletDialog(canvas_, this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->show();
}

void MainWindow::showPalette(const QPoint& globalPos) {
    palette_->setBrushes(brushes_, brushCombo_->currentIndex());
    palette_->setRecentColors(colorPanel_->recentColors());
    palette_->setColor(colorPanel_->foreground());
    palette_->popupAt(globalPos);
}

void MainWindow::mergeDown() {
    app::Document* doc = activeDocument();
    if (doc == nullptr || canvas_->strokeActive()) return;
    const Result<LayerId> r = app::mergeLayerDown(*doc, doc->layers().activeLayer());
    if (!r.ok()) {
        statusBar()->showMessage(QString::fromStdString(r.message()), 3000);
        return;
    }
    (void)doc->layers().setActiveLayer(r.value());
    layerPanel_->refresh();
    canvas_->invalidateCanvas();
    thumbTimer_->start();
    refreshTitle();
}

void MainWindow::togglePanels() {
    panelsHidden_ = !panelsHidden_;
    const bool show = !panelsHidden_;
    colorDock_->setVisible(show);
    layerDock_->setVisible(show);
    navDock_->setVisible(show);
    toolsBar_->setVisible(show);
    optionsBar_->setVisible(show);
}

// ── 상태 ─────────────────────────────────────────────────────────────────

void MainWindow::refreshStatus() {
    if (statusMain_ == nullptr || colorPanel_ == nullptr) {
        return; // 아직 짓는 중이다(툴바 초기값 설정이 시그널을 낸다)
    }
    app::Document* doc = activeDocument();
    QString layerName;
    if (doc != nullptr) {
        if (const LayerPtr l = doc->layers().find(doc->layers().activeLayer())) {
            layerName = QString::fromStdString(l->name());
        }
    }
    const char* toolName = "붓";
    const char* toolIcon = "brush";
    switch (canvas_->tool()) {
    case Tool::Brush: toolName = "붓"; toolIcon = "brush"; break;
    case Tool::Eraser: toolName = "지우개"; toolIcon = "eraser"; break;
    case Tool::Eyedropper: toolName = "스포이드"; toolIcon = "color-picker"; break;
    case Tool::Hand: toolName = "손"; toolIcon = "hand-stop"; break;
    case Tool::Fill: toolName = "채우기"; toolIcon = "bucket-droplet"; break;
    case Tool::SelectRect: toolName = "사각 선택"; toolIcon = "square-dashed"; break;
    case Tool::SelectEllipse: toolName = "타원 선택"; toolIcon = "circle-dashed"; break;
    case Tool::SelectLasso: toolName = "올가미"; toolIcon = "lasso"; break;
    case Tool::SelectWand: toolName = "마술봉"; toolIcon = "wand"; break;
    }
    if (statusToolIconName_ != toolIcon) {
        statusToolIconName_ = toolIcon;
        statusToolIcon_->setPixmap(themedIcon(toolIcon, 14).pixmap(14, 14));
    }
    statusMain_->setText(QString("%1 · %2 px · %3% · %4")
                             .arg(toolName)
                             .arg(sizeSpin_->value(), 0, 'f', 1)
                             .arg(opacitySlider_->value())
                             .arg(layerName));

    const mari::win::ViewState& v = canvas_->viewState();
    statusView_->setText(QString("%1% · %2°%3")
                             .arg(v.zoom * 100.0, 0, 'f', 0)
                             .arg(v.rotationDeg, 0, 'f', 0)
                             .arg(v.mirrorX ? " · 미러" : ""));

    const bool debug = debugStatusAction_ != nullptr && debugStatusAction_->isChecked();
    statusDebug_->setVisible(debug);
    if (debug) {
        QString rec = "기록 없음";
        if (doc != nullptr && doc->recorder() != nullptr) {
            rec = doc->recordingBroken() ? "기록 [고장]" : "저널 " + journalPath_;
        }
        const LatencyStats& lat = canvas_->latency();
        QString latText = "펜→화면 —";
        if (lat.samples > 0) {
            latText = QString("펜→화면 %1 ms (평균 %2 · 최대 %3 · >16ms %4/%5)")
                          .arg(lat.lastMs, 0, 'f', 1)
                          .arg(lat.avgMs, 0, 'f', 1)
                          .arg(lat.maxMs, 0, 'f', 1)
                          .arg(lat.over16)
                          .arg(lat.samples);
        }
        // 🔴 입력 API 는 항상 windows-ink 다. 다른 값이 뜨면 설계 위반이다(docs/03 3절).
        statusDebug_->setToolTip(QString("입력 %1 · %2").arg(mari::win::PointerInput::inputApiName()).arg(rec));
        statusDebug_->setText(QString("획 %1 · 기록 %2 · 롤백 %3 · %4")
                                  .arg(strokes_)
                                  .arg(recordedStrokes_)
                                  .arg(rolledBack_)
                                  .arg(latText));
    }
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
        title = (p.empty() ? QString("제목 없음") : QString::fromStdString(p)) + (doc->isSaved() ? "" : " *") +
                " — Mari Paint";
    }
    setWindowTitle(title);
    refreshStatus();
}

} // namespace mari::ui
