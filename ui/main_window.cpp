// Mari Paint — 최소 UI 구현 (ui/main_window.hpp)
#include "main_window.hpp"

#include "adjust_dialog.hpp"
#include "brush_editor.hpp"
#include "brush_panel.hpp"
#include "canvas_widget.hpp"
#include "color_panel.hpp"
#include "icons.hpp"
#include "layer_panel.hpp"
#include "navigator.hpp"
#include "new_document_dialog.hpp"
#include "popup_palette.hpp"
#include "shortcut_dialog.hpp"
#include "side_panels.hpp"
#include "tablet_dialog.hpp"

#include <mari/agent/brush_library.hpp>
#include <mari/app/layer_commands.hpp>
#include <mari/io/brush/importer.hpp>

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
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QTreeWidget>
#include <QFormLayout>
#include <QPushButton>
#include <QDialog>
#include <QDateTime>
#include <QDir>
#include <QStandardPaths>
#include <QUuid>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QVBoxLayout>
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
    builtinBrushCount_ = brushes_.size();
    brushDir_ = agent::defaultBrushDir();
    for (brush::MariBrushPreset& p : agent::loadPresetDir(brushDir_)) brushes_.push_back(std::move(p));

    canvas_ = new CanvasWidget(this);
    setCentralWidget(canvas_);
    canvas_->setStrokeConfigProvider([this] { return strokeConfig(); });

    buildMenus();
    buildToolbars();
    buildDocks();
    buildStatusBar();
    // 🔴 메뉴·툴바가 다 만들어진 뒤에 모아야 저장된 단축키 변경이 전부 입혀진다.
    shortcuts_ = std::make_unique<ShortcutRegistry>();
    shortcuts_->collect(menuBar(), {{"도구", toolsBar_}});
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
        markAutosaveDirty();
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
        gradientOptions_->setVisible(t == Tool::Gradient);
        refreshStatus();
    });
    connect(canvas_, &CanvasWidget::regionFilled, this, [this] {
        thumbTimer_->start();
        refreshTitle();
        markAutosaveDirty();
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
    setupAutosave();
    QTimer::singleShot(300, this, [this] {
        checkRecovery();
        showLanding();
    });

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
    file->addAction("자동 저장 · 백업 설정...", this, &MainWindow::showAutosaveSettings);
    file->addSeparator();
    file->addAction("종료(&Q)", QKeySequence::Quit, this, &QWidget::close);

    QMenu* edit = menuBar()->addMenu("편집(&E)");
    undoAction_ = edit->addAction(themedIcon("arrow-back-up", 20), "실행 취소(&U)", QKeySequence::Undo, this, &MainWindow::undo);
    redoAction_ = edit->addAction(themedIcon("arrow-forward-up", 20), "다시 실행(&R)", QKeySequence::Redo, this, &MainWindow::redo);
    // 포토샵: Ctrl+Shift+Z 다시 실행(Ctrl+Y 도 받는다)
    redoAction_->setShortcuts({QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Z), QKeySequence(Qt::CTRL | Qt::Key_Y)});
    edit->addSeparator();
    edit->addAction(themedIcon("arrows-maximize", 20), "자유 변형", QKeySequence(Qt::CTRL | Qt::Key_T), this,
                    &MainWindow::beginFreeTransform);
    edit->addSeparator();
    edit->addAction("전경색으로 채우기", QKeySequence(Qt::ALT | Qt::Key_Backspace), this,
                    [this] { canvas_->fillSelection(colorPanel_->foreground(), false); });
    edit->addAction("배경색으로 채우기", QKeySequence(Qt::CTRL | Qt::Key_Backspace), this,
                    [this] { canvas_->fillSelection(colorPanel_->background(), false); });
    edit->addAction("선택 영역 지우기", QKeySequence(Qt::Key_Delete), this,
                    [this] { canvas_->fillSelection(Qt::black, true); });
    edit->addSeparator();
    edit->addAction("붓 크게", QKeySequence(Qt::Key_BracketRight), this, [this] { stepBrushSize(+1); });
    edit->addAction("붓 작게", QKeySequence(Qt::Key_BracketLeft), this, [this] { stepBrushSize(-1); });
    edit->addAction("불투명도 +", QKeySequence(Qt::SHIFT | Qt::Key_BracketRight), this, [this] { stepOpacity(+1); });
    edit->addAction("불투명도 −", QKeySequence(Qt::SHIFT | Qt::Key_BracketLeft), this, [this] { stepOpacity(-1); });
    edit->addSeparator();
    edit->addAction("전경↔배경 교환", QKeySequence(Qt::Key_X), this, [this] { colorPanel_->swap(); });
    edit->addAction("기본 색(검정/흰색)", QKeySequence(Qt::Key_D), this, [this] { colorPanel_->resetDefaults(); });
    edit->addSeparator();
    edit->addAction(themedIcon("keyboard", 20), "단축키 설정...", QKeySequence(Qt::CTRL | Qt::ALT | Qt::SHIFT | Qt::Key_K),
                    this, &MainWindow::showShortcutDialog);

    QMenu* select = menuBar()->addMenu("선택(&S)");
    select->addAction("전체 선택", QKeySequence::SelectAll, this, [this] { canvas_->selectAll(); });
    select->addAction("선택 해제", QKeySequence(Qt::CTRL | Qt::Key_D), this, [this] { canvas_->deselect(); });
    select->addAction("선택 반전", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_I), this,
                      [this] { canvas_->invertSelection(); });
    select->addSeparator();
    select->addAction("수식키: Shift 더하기 · Alt 빼기 · Shift+Alt 교집합")->setEnabled(false);

    QMenu* image = menuBar()->addMenu("이미지(&I)");
    QMenu* adj = image->addMenu("보정");
    adj->addAction("밝기/대비...", this, [this] { runAdjust(app::AdjustKind::BrightnessContrast); });
    adj->addAction("레벨...", QKeySequence(Qt::CTRL | Qt::Key_L), this, [this] { runAdjust(app::AdjustKind::Levels); });
    adj->addAction("곡선...", QKeySequence(Qt::CTRL | Qt::Key_M), this, [this] { runAdjust(app::AdjustKind::Curves); });
    adj->addAction("색조/채도...", QKeySequence(Qt::CTRL | Qt::Key_U), this, [this] { runAdjust(app::AdjustKind::HueSaturation); });
    adj->addSeparator();
    adj->addAction("반전", QKeySequence(Qt::CTRL | Qt::Key_I), this, [this] { runAdjust(app::AdjustKind::Invert); });
    adj->addAction("채도 제거", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_U), this, [this] { runAdjust(app::AdjustKind::Desaturate); });
    adj->addAction("문턱값...", this, [this] { runAdjust(app::AdjustKind::Threshold); });
    adj->addAction("포스터화...", this, [this] { runAdjust(app::AdjustKind::Posterize); });
    image->addSeparator();
    image->addAction("이미지 크기...", QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_I), this, [this] { showImageSizeDialog(false); });
    image->addAction("캔버스 크기...", QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_C), this, [this] { showImageSizeDialog(true); });
    image->addAction("선택 영역으로 자르기", this, [this] {
        runCanvasOp([](app::Document& d) {
            if (d.selectionMask().isAll()) return Result<void>(Err("선택이 없다", ErrorCode::InvalidArgument));
            return app::cropCanvas(d, StrokeSource::humanPen(), d.selectionMask().bounds());
        });
    });
    image->addSeparator();
    QMenu* rot = image->addMenu("이미지 회전");
    rot->addAction("시계 방향 90°", this, [this] { runCanvasOp([](app::Document& d) { return app::rotateCanvas(d, StrokeSource::humanPen(), 1); }); });
    rot->addAction("반시계 방향 90°", this, [this] { runCanvasOp([](app::Document& d) { return app::rotateCanvas(d, StrokeSource::humanPen(), 3); }); });
    rot->addAction("180°", this, [this] { runCanvasOp([](app::Document& d) { return app::rotateCanvas(d, StrokeSource::humanPen(), 2); }); });
    rot->addSeparator();
    rot->addAction("캔버스 좌우 뒤집기", this, [this] { runCanvasOp([](app::Document& d) { return app::flipCanvas(d, StrokeSource::humanPen(), true); }); });
    rot->addAction("캔버스 상하 뒤집기", this, [this] { runCanvasOp([](app::Document& d) { return app::flipCanvas(d, StrokeSource::humanPen(), false); }); });

    QMenu* brushMenu = menuBar()->addMenu("브러시(&B)");
    brushMenu->addAction(themedIcon("folder-open", 20), "브러시 가져오기... (.abr · .sut · .mbp)", this, &MainWindow::importBrushes);
    brushMenu->addAction(themedIcon("settings", 20), "현재 브러시 편집...", QKeySequence(Qt::Key_F5), this,
                         [this] { editBrush(brushCombo_->currentIndex(), false); });
    brushMenu->addAction(themedIcon("square-plus", 20), "새 브러시(현재 브러시 바탕)...", this,
                         [this] { editBrush(brushCombo_->currentIndex(), true); });
    brushMenu->addAction(themedIcon("copy", 20), "현재 브러시 복제", this, [this] { duplicateBrush(brushCombo_->currentIndex()); });
    brushMenu->addAction(themedIcon("trash", 20), "현재 브러시 삭제", this, [this] { deleteBrush(brushCombo_->currentIndex()); });
    brushMenu->addSeparator();
    brushMenu->addAction("브러시 폴더 다시 읽기", this, [this] { reloadBrushes(); });

    QMenu* layer = menuBar()->addMenu("레이어(&L)");
    // 기본 단축키는 포토샵과 같다(편집 › 단축키 설정 에서 바꾼다).
    QAction* newLayer = layer->addAction(themedIcon("square-plus", 20), "새 레이어", this, [this] { layerPanel_->addLayer(); });
    newLayer->setShortcuts({QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_N), QKeySequence(Qt::Key_Insert)});
    layer->addAction(themedIcon("folder-plus", 20), "새 그룹", this, [this] { layerPanel_->addGroup(); });
    layer->addAction(themedIcon("copy", 20), "레이어 복제", QKeySequence(Qt::CTRL | Qt::Key_J), this,
                     [this] { layerPanel_->duplicateLayer(); });
    layer->addAction(themedIcon("trash", 20), "레이어 삭제", this, [this] { layerPanel_->removeLayer(); });
    layer->addSeparator();
    layer->addAction(themedIcon("folder", 20), "그룹으로 묶기", QKeySequence(Qt::CTRL | Qt::Key_G), this,
                     [this] { layerPanel_->groupActive(); });
    layer->addAction("그룹 풀기", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_G), this,
                     [this] { layerPanel_->ungroupActive(); });
    layer->addAction(themedIcon("arrow-bar-to-down", 20), "클리핑 마스크 만들기/해제", QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_G),
                     this, [this] { layerPanel_->toggleClipActive(); });
    QMenu* mask = layer->addMenu(themedIcon("mask", 20), "레이어 마스크");
    mask->addAction("전부 보이기", this, [this] { layerPanel_->addMask(false); });
    mask->addAction("선택 영역 보이기", this, [this] { layerPanel_->addMask(true); });
    mask->addSeparator();
    mask->addAction("선택을 마스크에 보이기", this, [this] { layerPanel_->paintMaskWithSelection(true); });
    mask->addAction("선택을 마스크에서 가리기", this, [this] { layerPanel_->paintMaskWithSelection(false); });
    mask->addSeparator();
    mask->addAction("마스크 적용", this, [this] { layerPanel_->applyMask(); });
    mask->addAction("마스크 삭제", this, [this] { layerPanel_->removeMask(); });
    layer->addSeparator();
    layer->addAction(themedIcon("arrow-merge", 20), "아래와 병합", QKeySequence(Qt::CTRL | Qt::Key_E), this,
                     &MainWindow::mergeDown);
    layer->addAction(themedIcon("stack-2", 20), "이미지 평탄화", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_E), this,
                     &MainWindow::flattenImage);
    layer->addSeparator();
    layer->addAction(themedIcon("arrow-up", 20), "레이어 위로", QKeySequence(Qt::CTRL | Qt::Key_BracketRight), this,
                     [this] { layerPanel_->moveActive(+1); });
    layer->addAction(themedIcon("arrow-down", 20), "레이어 아래로", QKeySequence(Qt::CTRL | Qt::Key_BracketLeft), this,
                     [this] { layerPanel_->moveActive(-1); });
    QAction* selUp = layer->addAction("위 레이어 선택", this, [this] { layerPanel_->selectAdjacent(+1); });
    selUp->setShortcuts({QKeySequence(Qt::ALT | Qt::Key_BracketRight), QKeySequence(Qt::Key_PageUp)});
    QAction* selDown = layer->addAction("아래 레이어 선택", this, [this] { layerPanel_->selectAdjacent(-1); });
    selDown->setShortcuts({QKeySequence(Qt::ALT | Qt::Key_BracketLeft), QKeySequence(Qt::Key_PageDown)});

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
    viewActions_.rotL = view->addAction(themedIcon("rotate", 20), "왼쪽으로 회전", QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_BracketLeft),
                                        this, [this] { canvas_->rotateBy(-15.0); });
    viewActions_.rotR = view->addAction(themedIcon("rotate-clockwise", 20), "오른쪽으로 회전",
                                        QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_BracketRight), this, [this] { canvas_->rotateBy(15.0); });
    view->addAction("회전 초기화", QKeySequence(Qt::Key_5), this, [this] { canvas_->resetRotation(); });
    view->addSeparator();
    symmetryV_ = view->addAction(themedIcon("symmetry", 20), "좌우 대칭 그리기", QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_S), this, [this] {
        canvas_->setSymmetry((symmetryV_->isChecked() ? 1 : 0) | (symmetryH_->isChecked() ? 2 : 0));
    });
    symmetryV_->setCheckable(true);
    symmetryH_ = view->addAction("상하 대칭 그리기", this, [this] {
        canvas_->setSymmetry((symmetryV_->isChecked() ? 1 : 0) | (symmetryH_->isChecked() ? 2 : 0));
    });
    symmetryH_->setCheckable(true);
    gridAction_ = view->addAction(themedIcon("grid", 20), "격자", QKeySequence(Qt::CTRL | Qt::Key_Apostrophe), this, [this] {
        canvas_->setGrid(gridAction_->isChecked(), QSettings().value("grid/spacing", 64).toInt());
    });
    gridAction_->setCheckable(true);
    view->addAction("격자 간격...", this, [this] {
        bool ok = false;
        const int v = QInputDialog::getInt(this, "격자", "간격(px)", QSettings().value("grid/spacing", 64).toInt(), 2, 4096, 1, &ok);
        if (!ok) return;
        QSettings().setValue("grid/spacing", v);
        canvas_->setGrid(gridAction_->isChecked(), v);
    });
    view->addSeparator();
    viewActions_.mirror = view->addAction(themedIcon("flip-horizontal", 20), "미러 보기", QKeySequence(Qt::ALT | Qt::Key_M), this,
                                          [this] { canvas_->toggleMirror(); });
    view->addSeparator();
    panelsMenu_ = view->addMenu("패널"); // buildDocks() 가 도크마다 켜기/끄기 항목을 채운다
    view->addAction("참조 이미지 열기...", this, [this] { refDock_->setVisible(true); refDock_->raise(); refPanel_->open(); });
    viewActions_.panels = view->addAction(themedIcon("layout-sidebar-right-collapse", 20), "패널 숨김/표시",
                                          QKeySequence(Qt::Key_Tab), this, &MainWindow::togglePanels);
    QAction* canvasOnly = view->addAction("캔버스 전용 모드", this, &MainWindow::toggleCanvasOnly);
    canvasOnly->setShortcuts({QKeySequence(Qt::Key_F), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F)});
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
    addTool("square-dashed", "사각 선택", "사각형 선택 (M) — Shift 더하기 · Alt 빼기 · Shift+Alt 교집합", Qt::Key_M, Tool::SelectRect);
    addTool("circle-dashed", "타원 선택", "타원 선택 (Shift+M)", Qt::Key_O, Tool::SelectEllipse)
        ->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_M));
    addTool("lasso", "올가미", "올가미 선택 (L)", Qt::Key_L, Tool::SelectLasso);
    addTool("wand", "마술봉", "마술봉 (W) — 이어진 같은 색 영역", Qt::Key_W, Tool::SelectWand);
    toolsBar_->addSeparator();
    addTool("line", "직선", "직선 (U) — 현재 붓으로 긋는다. Shift: 45°", Qt::Key_U, Tool::Line);
    addTool("rectangle", "사각형", "사각형 (Shift+U) — 현재 붓으로 테두리. Shift: 정사각형", Qt::Key_U, Tool::Rectangle)
        ->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_U));
    addTool("circle", "타원", "타원 (Ctrl+U 아님 — 도구상자에서) — 현재 붓으로 테두리. Shift: 정원", Qt::Key_unknown, Tool::Ellipse)
        ->setShortcut(QKeySequence());
    addTool("gradient", "그라데이션", "그라데이션 (Shift+G) — 전경→배경(또는 투명), 선택 안에", Qt::Key_G, Tool::Gradient)
        ->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_G));

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
    // 기본은 "약함" — 끔이면 손떨림이 잔물결로 그대로 남는다(실기 확인). 마지막 선택을 기억한다.
    smoothingCombo_->setCurrentIndex(std::clamp(QSettings().value("stroke/smoothing", 1).toInt(), 0, 3));
    connect(smoothingCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [](int i) { QSettings().setValue("stroke/smoothing", i); });
    optionsBar_->addWidget(smoothingCombo_);
    optionsBar_->addWidget(new QLabel("데드존"));
    deadZoneSpin_ = new QSpinBox(optionsBar_);
    deadZoneSpin_->setRange(0, 40);
    deadZoneSpin_->setSuffix(" px");
    deadZoneSpin_->setFixedWidth(64);
    deadZoneSpin_->setToolTip("끈에 매단 펜: 커서가 이 반지름(화면 px)을 벗어나야 선이 따라온다. 느린 떨림을 없앤다. 보정이 켜졌을 때만.");
    deadZoneSpin_->setValue(QSettings().value("stabilizer/deadZone", 0).toInt());
    connect(deadZoneSpin_, &QSpinBox::valueChanged, this, [](int v) { QSettings().setValue("stabilizer/deadZone", v); });
    optionsBar_->addWidget(deadZoneSpin_);

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

    // 그라데이션 옵션(그라데이션 도구일 때만)
    gradientOptions_ = new QWidget(optionsBar_);
    {
        auto* row = new QHBoxLayout(gradientOptions_);
        row->setContentsMargins(6, 0, 0, 0);
        row->setSpacing(6);
        auto* kind = new QComboBox(gradientOptions_);
        kind->addItems({"선형", "원형"});
        auto* to = new QComboBox(gradientOptions_);
        to->addItems({"전경 → 배경", "전경 → 투명"});
        row->addWidget(kind);
        row->addWidget(to);
        const auto push = [this, kind, to] { canvas_->setGradientOptions(kind->currentIndex() == 1, to->currentIndex() == 1); };
        connect(kind, &QComboBox::currentIndexChanged, this, [push](int) { push(); });
        connect(to, &QComboBox::currentIndexChanged, this, [push](int) { push(); });
        push();
    }
    optionsBar_->addWidget(gradientOptions_);
    gradientOptions_->setVisible(false);

    // 자유 변형 옵션(변형 중일 때만)
    transformOptions_ = new QWidget(optionsBar_);
    {
        auto* row = new QHBoxLayout(transformOptions_);
        row->setContentsMargins(6, 0, 0, 0);
        row->setSpacing(4);
        const auto spin = [&](const char* label, double lo, double hi, double v, const char* suffix) {
            row->addWidget(new QLabel(label, transformOptions_));
            auto* sp = new QDoubleSpinBox(transformOptions_);
            sp->setRange(lo, hi);
            sp->setDecimals(1);
            sp->setValue(v);
            sp->setSuffix(suffix);
            sp->setFixedWidth(84);
            row->addWidget(sp);
            connect(sp, &QDoubleSpinBox::valueChanged, this, [this](double) {
                if (xfSyncing_) return;
                canvas_->setTransformParams(xfDx_->value(), xfDy_->value(), xfSx_->value() / 100.0, xfSy_->value() / 100.0, xfRot_->value());
            });
            return sp;
        };
        xfDx_ = spin("X", -100000, 100000, 0, " px");
        xfDy_ = spin("Y", -100000, 100000, 0, " px");
        xfSx_ = spin("W", -6400, 6400, 100, " %");
        xfSy_ = spin("H", -6400, 6400, 100, " %");
        xfRot_ = spin("각도", -3600, 3600, 0, "°");
        auto* flipH = new QPushButton("좌우", transformOptions_);
        auto* flipV = new QPushButton("상하", transformOptions_);
        auto* ok = new QPushButton("적용 (Enter)", transformOptions_);
        auto* cancel = new QPushButton("취소 (Esc)", transformOptions_);
        connect(flipH, &QPushButton::clicked, this, [this] { canvas_->transformFlip(true); });
        connect(flipV, &QPushButton::clicked, this, [this] { canvas_->transformFlip(false); });
        connect(ok, &QPushButton::clicked, this, [this] { canvas_->commitTransform(); });
        connect(cancel, &QPushButton::clicked, this, [this] { canvas_->cancelTransform(); });
        row->addWidget(flipH);
        row->addWidget(flipV);
        row->addWidget(ok);
        row->addWidget(cancel);
    }
    optionsBar_->addWidget(transformOptions_);
    transformOptions_->setVisible(false);
    connect(canvas_, &CanvasWidget::transformChanged, this,
            [this](bool active, double dx, double dy, double sx, double sy, double rot) {
                transformOptions_->setVisible(active);
                xfSyncing_ = true;
                xfDx_->setValue(dx); xfDy_->setValue(dy); xfSx_->setValue(sx * 100.0); xfSy_->setValue(sy * 100.0); xfRot_->setValue(rot);
                xfSyncing_ = false;
                if (!active) afterPixelChange();
            });

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
    optionsBar_->addSeparator();
    optionsBar_->addAction(symmetryV_);
    optionsBar_->addAction(gridAction_);
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
    // 🔴 도크는 전부 같은 자유도다: 끌어 옮기기 · 떼어 띄우기 · 닫기, 어느 가장자리든.
    //    나란히(중첩) 붙이기도 허용해 열 두 개로 벌릴 수 있다. 닫은 건 보기 → 패널 메뉴로 되살린다.
    constexpr QDockWidget::DockWidgetFeatures kDockFeatures =
        QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable | QDockWidget::DockWidgetClosable;
    setDockOptions(QMainWindow::AnimatedDocks | QMainWindow::AllowNestedDocks | QMainWindow::AllowTabbedDocks);
    colorDock_ = new QDockWidget("색", this);
    colorDock_->setFeatures(kDockFeatures);
    colorPanel_ = new ColorPanel(colorDock_);
    colorDock_->setWidget(colorPanel_);
    addDockWidget(Qt::RightDockWidgetArea, colorDock_);

    layerDock_ = new QDockWidget("레이어", this);
    layerDock_->setFeatures(kDockFeatures);
    layerPanel_ = new LayerPanel(layerDock_);
    layerDock_->setWidget(layerPanel_);
    addDockWidget(Qt::RightDockWidgetArea, layerDock_);
    navDock_ = new QDockWidget("내비게이터", this);
    navDock_->setObjectName("navDock");
    navDock_->setFeatures(kDockFeatures);
    navigator_ = new Navigator(canvas_, navDock_);
    navDock_->setWidget(navigator_);
    addDockWidget(Qt::RightDockWidgetArea, navDock_);
    colorDock_->setObjectName("colorDock");
    layerDock_->setObjectName("layerDock");
    brushDock_ = new QDockWidget("브러시", this);
    brushDock_->setObjectName("brushDock");
    brushDock_->setFeatures(kDockFeatures);
    brushPanel_ = new BrushPanel(brushDock_);
    brushDock_->setWidget(brushPanel_);
    addDockWidget(Qt::RightDockWidgetArea, brushDock_);
    tabifyDockWidget(layerDock_, brushDock_);
    historyDock_ = new QDockWidget("히스토리", this);
    historyDock_->setObjectName("historyDock");
    historyDock_->setFeatures(kDockFeatures);
    historyPanel_ = new HistoryPanel(historyDock_);
    historyDock_->setWidget(historyPanel_);
    addDockWidget(Qt::RightDockWidgetArea, historyDock_);
    tabifyDockWidget(layerDock_, historyDock_);
    paletteDock_ = new QDockWidget("팔레트", this);
    paletteDock_->setObjectName("paletteDock");
    paletteDock_->setFeatures(kDockFeatures);
    palettePanel_ = new PalettePanel(paletteDock_);
    paletteDock_->setWidget(palettePanel_);
    addDockWidget(Qt::RightDockWidgetArea, paletteDock_);
    tabifyDockWidget(colorDock_, paletteDock_);
    colorDock_->raise();
    refDock_ = new QDockWidget("참조", this);
    refDock_->setObjectName("refDock");
    refDock_->setFeatures(kDockFeatures);
    refPanel_ = new ReferencePanel(refDock_);
    refDock_->setWidget(refPanel_);
    addDockWidget(Qt::RightDockWidgetArea, refDock_);
    tabifyDockWidget(navDock_, refDock_);
    navDock_->raise();
    layerDock_->raise();
    connect(historyPanel_, &HistoryPanel::jumpRequested, this, [this](int steps) {
        app::Document* doc = activeDocument();
        if (doc == nullptr || canvas_->strokeActive() || canvas_->transformActive()) return;
        for (int i = 0; i < std::abs(steps); ++i) {
            const Result<void> r = steps > 0 ? doc->undo() : doc->redo();
            if (!r.ok()) break;
        }
        canvas_->invalidateCanvas();
        canvas_->selectionChangedExternally();
        layerPanel_->refresh();
        thumbTimer_->start();
        refreshTitle();
        historyPanel_->refresh();
    });
    connect(palettePanel_, &PalettePanel::colorChosen, this, [this](const QColor& c) { colorPanel_->setForeground(c); });
    connect(refPanel_, &ReferencePanel::colorPicked, this, [this](const QColor& c) { colorPanel_->setForeground(c); });
    resizeDocks({navDock_, colorDock_, layerDock_}, {150, 330, 400}, Qt::Vertical);
    resizeDocks({colorDock_}, {280}, Qt::Horizontal);
    for (QDockWidget* d : {colorDock_, paletteDock_, layerDock_, brushDock_, historyDock_, navDock_, refDock_}) {
        d->setAllowedAreas(Qt::AllDockWidgetAreas);
        panelsMenu_->addAction(d->toggleViewAction());
    }
    panelsMenu_->addSeparator();
    panelsMenu_->addAction("패널 배치 초기화", this, [this] {
        QSettings().remove("window/state");
        QMessageBox::information(this, "패널 배치", "다음에 켤 때 기본 배치로 돌아갑니다.");
    });
    brushPanel_->setPreviewColor(colorPanel_->foreground());
    brushPanel_->setBrushes(brushes_, builtinBrushCount_, brushCombo_->currentIndex());
    connect(brushPanel_, &BrushPanel::brushSelected, this, [this](int i) { brushCombo_->setCurrentIndex(i); });
    connect(brushPanel_, &BrushPanel::editRequested, this, [this](int i) { editBrush(i, false); });
    connect(brushPanel_, &BrushPanel::duplicateRequested, this, &MainWindow::duplicateBrush);
    connect(brushPanel_, &BrushPanel::deleteRequested, this, &MainWindow::deleteBrush);
    connect(brushPanel_, &BrushPanel::newRequested, this, [this] { editBrush(brushCombo_->currentIndex(), true); });
    connect(brushPanel_, &BrushPanel::importRequested, this, &MainWindow::importBrushes);
    connect(brushCombo_, &QComboBox::currentIndexChanged, this, [this](int i) { brushPanel_->setCurrent(i); });

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
    connect(colorPanel_, &ColorPanel::foregroundChanged, this, [this](const QColor& c) {
        brushPanel_->setPreviewColor(c);
        palettePanel_->setCurrentColor(c);
        canvas_->setBackgroundColor(colorPanel_->background());
        refreshStatus();
    });
    canvas_->setBackgroundColor(colorPanel_->background());
    palettePanel_->setCurrentColor(colorPanel_->foreground());
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
    autosaveId_ = QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
    autosaveDirty_ = false;
    canvas_->setDocument(doc);
    layerPanel_->setDocument(doc);
    if (historyPanel_ != nullptr) historyPanel_->setDocument(doc);
    if (navigator_ != nullptr) navigator_->refreshImage();
    refreshTitle();
    refreshStatus();
}

void MainWindow::newDocument() {
    if (!confirmDiscard()) return;
    NewDocumentDialog dlg(this, /*landing=*/false);
    if (dlg.exec() != QDialog::Accepted) return;
    createDocumentFromDialog(dlg);
}

void MainWindow::createDocumentFromDialog(const NewDocumentDialog& dlg) {
    if (app::Document* old = activeDocument()) {
        canvas_->setDocument(nullptr);
        layerPanel_->setDocument(nullptr);
        (void)app_.closeDocument(old, false);
    }
    Result<app::IDocumentBridge*> made = app_.createDocument(dlg.canvasWidth(), dlg.canvasHeight(), dlg.background());
    if (!made.ok()) {
        QMessageBox::warning(this, "새 문서", QString::fromStdString(made.message()));
        attachDocument(activeDocument());
        return;
    }
    attachDocument(static_cast<app::Document*>(made.value()));
}

void MainWindow::showLanding() {
    // 시작 화면: 복구할 게 없거나 복구를 안 했으면 새 문서를 묻는다. 취소하면 문서 없이 뜬다 —
    // 파일 → 새 문서/열기로 시작하면 된다.
    if (activeDocument() != nullptr) return;
    NewDocumentDialog dlg(this, /*landing=*/true);
    if (dlg.exec() != QDialog::Accepted) return;
    createDocumentFromDialog(dlg);
}

void MainWindow::openDocument() {
    if (!confirmDiscard()) return;
    const QString path = QFileDialog::getOpenFileName(this, "열기", QString(), "그림 (*.ora *.psd);;OpenRaster (*.ora);;Photoshop (*.psd)");
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
    if (forceDialog || path.empty() || recovered_) {
        const QString chosen = QFileDialog::getSaveFileName(this, "저장", QString(), "OpenRaster (*.ora);;Photoshop (*.psd)");
        if (chosen.isEmpty()) return false;
        path = chosen.toStdString();
        const bool isOra = path.size() >= 4 && path.substr(path.size() - 4) == ".ora";
        const bool isPsd = path.size() >= 4 && path.substr(path.size() - 4) == ".psd";
        if (!isOra && !isPsd) path += ".ora";
    }
    // 백업: 덮어쓰기 전 원본을 .bak 로(설정으로 끈다).
    if (QSettings().value("backup/enabled", true).toBool()) {
        const QString q = QString::fromStdString(path);
        if (QFile::exists(q)) {
            QFile::remove(q + ".bak");
            QFile::copy(q, q + ".bak");
        }
    }
    const bool psdOut = path.size() >= 4 && path.substr(path.size() - 4) == ".psd";
    if (psdOut) statusBar()->showMessage("PSD 는 교환 포맷 — 과정 기록(prooflog)은 .ora 에만 담긴다", 6000);
    const Result<void> saved = doc->saveAs(path, psdOut ? "psd" : "ora");
    if (!saved.ok()) {
        QMessageBox::warning(this, "저장", QString::fromStdString(saved.message()));
        return false;
    }
    recovered_ = false;
    clearAutosave();
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
    clearAutosave();
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
    const QColor bgc = colorPanel_->background();
    cfg.background = Color8::rgba(static_cast<u8>(bgc.red()), static_cast<u8>(bgc.green()), static_cast<u8>(bgc.blue()), 255);
    cfg.eraser = canvas_->tool() == Tool::Eraser;
    switch (smoothingCombo_->currentIndex()) {
    case 1: cfg.smoothing = stroke::SmoothingMode::Light; break;
    case 2: cfg.smoothing = stroke::SmoothingMode::Medium; break;
    case 3: cfg.smoothing = stroke::SmoothingMode::Strong; break;
    default: cfg.smoothing = stroke::SmoothingMode::Off; break;
    }
    // 데드존은 화면 px 로 고른다(줌과 무관한 손 느낌) → 캔버스 px 로.
    const f64 zoom = std::max(canvas_->viewState().zoom, 1e-3);
    cfg.deadZone = static_cast<f32>(deadZoneSpin_->value() * canvas_->devicePixelRatioF() / zoom);
    cfg.endCorrection = true;
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
    // 🔴 변형 중에는 막는다 — 적용/취소가 "변형 준비" 항목을 되돌리는데, 그 사이에 스택이 움직이면 엉뚱한 걸 되돌린다.
    if (doc == nullptr || canvas_->strokeActive() || canvas_->transformActive()) return;
    const Result<void> r = doc->undo();
    if (!r.ok()) statusBar()->showMessage(QString::fromStdString(r.message()), 3000);
    canvas_->invalidateCanvas();
    layerPanel_->refresh();
    thumbTimer_->start();
    refreshTitle();
}

void MainWindow::redo() {
    app::Document* doc = activeDocument();
    if (doc == nullptr || canvas_->strokeActive() || canvas_->transformActive()) return;
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

void MainWindow::flattenImage() {
    app::Document* doc = activeDocument();
    if (doc == nullptr || canvas_->strokeActive()) return;
    layerPanel_->flattenAll();
    canvas_->invalidateCanvas();
    thumbTimer_->start();
    refreshTitle();
}

void MainWindow::showShortcutDialog() {
    ShortcutDialog dlg(*shortcuts_, this);
    dlg.exec();
}

// ── 브러시 라이브러리 ─────────────────────────────────────────────────────

void MainWindow::reloadBrushes(const QString& selectName) {
    const QString keep = selectName.isEmpty() ? brushCombo_->currentText() : selectName;
    brushes_ = brush::builtinPresets();
    builtinBrushCount_ = brushes_.size();
    std::vector<std::string> skipped;
    for (brush::MariBrushPreset& p : agent::loadPresetDir(brushDir_, &skipped)) brushes_.push_back(std::move(p));
    for (const std::string& sk : skipped) statusBar()->showMessage("브러시 파일 건너뜀: " + QString::fromStdString(sk), 6000);
    const QSignalBlocker block(brushCombo_);
    brushCombo_->clear();
    int idx = 0;
    for (usize i = 0; i < brushes_.size(); ++i) {
        brushCombo_->addItem(QString::fromStdString(brushes_[i].name));
        if (QString::fromStdString(brushes_[i].name) == keep) idx = static_cast<int>(i);
    }
    brushCombo_->setCurrentIndex(idx);
    brushPanel_->setBrushes(brushes_, builtinBrushCount_, idx);
    palette_->setBrushes(brushes_, idx);
}

void MainWindow::importBrushes() {
    const QStringList paths = QFileDialog::getOpenFileNames(this, "브러시 가져오기", QString(),
                                                            "브러시 (*.abr *.sut *.mbp);;Photoshop (*.abr);;Clip Studio (*.sut);;Mari (*.mbp)");
    if (paths.isEmpty()) return;
    std::vector<std::string> notes;
    std::vector<std::string> names;
    for (const QString& qp : paths) {
        const std::string path = qp.toStdString();
        std::vector<brush::MariBrushPreset> presets;
        if (qp.endsWith(".mbp", Qt::CaseInsensitive)) {
            Result<brush::MariBrushPreset> p = agent::loadPresetFile(path);
            if (!p.ok()) { notes.push_back(path + ": " + p.message()); continue; }
            presets.push_back(std::move(p).value());
        } else {
            Result<brush::ImportResult> r = io::brush::importBrushFile(path);
            if (!r.ok()) { notes.push_back(path + ": " + r.message()); continue; }
            for (const brush::ImportNote& n : r.value().report.notes) {
                const char* sev = n.severity == brush::ImportSeverity::Dropped ? "[버림] "
                                  : n.severity == brush::ImportSeverity::Degraded ? "[근사] " : "[정보] ";
                notes.push_back(sev + n.message);
            }
            presets = std::move(r.value().presets);
        }
        for (brush::MariBrushPreset& p : presets) {
            (void)agent::removePresetFile(brushDir_, p.name); // 같은 이름은 덮어쓴다 — 두 번 가져와도 하나
            const Result<void> w = agent::savePresetFile(agent::presetFilePath(brushDir_, p.name), p);
            if (!w.ok()) notes.push_back("저장 실패: " + w.message());
            else names.push_back(p.name);
        }
    }
    reloadBrushes(names.empty() ? QString() : QString::fromStdString(names.front()));
    showImportReport(QString("브러시 %1개 가져옴").arg(names.size()), notes, names);
}

void MainWindow::showImportReport(const QString& title, const std::vector<std::string>& notes,
                                  const std::vector<std::string>& importedNames) {
    QDialog dlg(this);
    dlg.setWindowTitle(title);
    dlg.resize(640, 420);
    auto* layout = new QVBoxLayout(&dlg);
    QString head = importedNames.empty() ? "가져온 브러시가 없다." : "가져온 브러시: ";
    for (usize i = 0; i < importedNames.size() && i < 12; ++i) head += (i ? ", " : "") + QString::fromStdString(importedNames[i]);
    if (importedNames.size() > 12) head += QString(" 외 %1개").arg(importedNames.size() - 12);
    auto* headLabel = new QLabel(head, &dlg);
    headLabel->setWordWrap(true);
    layout->addWidget(headLabel);
    auto* note = new QLabel(notes.empty() ? "번역하지 못한 항목이 없다 — 전부 그대로 가져왔다."
                                          : "번역하지 못했거나 근사한 항목(조용히 버리지 않는다 — docs/02 5절):", &dlg);
    layout->addWidget(note);
    auto* tree = new QTreeWidget(&dlg);
    tree->setHeaderHidden(true);
    tree->setRootIsDecorated(false);
    for (const std::string& n : notes) new QTreeWidgetItem(tree, {QString::fromStdString(n)});
    layout->addWidget(tree, 1);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok, &dlg);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    layout->addWidget(buttons);
    dlg.exec();
}

void MainWindow::editBrush(int index, bool forceCopy) {
    if (index < 0 || static_cast<usize>(index) >= brushes_.size()) return;
    const bool builtin = static_cast<usize>(index) < builtinBrushCount_;
    brush::MariBrushPreset base = brushes_[static_cast<usize>(index)];
    if (forceCopy || builtin) base.name += " 사본";
    // 옵션 바의 현재 크기·불투명도를 출발점으로.
    base.tip.diameter = static_cast<f32>(sizeSpin_->value());
    base.opacity = static_cast<f32>(opacitySlider_->value()) / 100.0f;
    BrushEditor dlg(base, builtin || forceCopy, colorPanel_->foreground(), colorPanel_->background(), this);
    if (dlg.exec() != QDialog::Accepted) return;
    brush::MariBrushPreset p = dlg.result();
    if (p.sourceFormat.empty() || p.sourceFormat == "builtin") p.sourceFormat = "native";
    const std::string oldName = brushes_[static_cast<usize>(index)].name;
    if (!dlg.saveAsNew() && !builtin) {
        // 원본 파일을 지우고(이름이 바뀌었을 수 있다) 새로 쓴다.
        (void)agent::removePresetFile(brushDir_, oldName);
    }
    (void)agent::removePresetFile(brushDir_, p.name); // 같은 이름은 덮어쓴다
    const Result<void> w = agent::savePresetFile(agent::presetFilePath(brushDir_, p.name), p);
    if (!w.ok()) {
        QMessageBox::warning(this, "브러시", QString::fromStdString(w.message()));
        return;
    }
    reloadBrushes(QString::fromStdString(p.name));
    statusBar()->showMessage("브러시 저장: " + QString::fromStdString(p.name), 3000);
}

void MainWindow::duplicateBrush(int index) {
    if (index < 0 || static_cast<usize>(index) >= brushes_.size()) return;
    brush::MariBrushPreset p = brushes_[static_cast<usize>(index)];
    p.name += " 사본";
    if (p.sourceFormat.empty() || p.sourceFormat == "builtin") p.sourceFormat = "native";
    const Result<void> w = agent::savePresetFile(agent::presetFilePath(brushDir_, p.name), p);
    if (!w.ok()) {
        QMessageBox::warning(this, "브러시", QString::fromStdString(w.message()));
        return;
    }
    reloadBrushes(QString::fromStdString(p.name));
}

void MainWindow::deleteBrush(int index) {
    if (index < 0 || static_cast<usize>(index) >= brushes_.size()) return;
    if (static_cast<usize>(index) < builtinBrushCount_) {
        statusBar()->showMessage("내장 브러시는 지울 수 없다", 3000);
        return;
    }
    const std::string name = brushes_[static_cast<usize>(index)].name;
    if (QMessageBox::question(this, "브러시 삭제", QString("'%1' 을 지울까? (파일도 지운다)").arg(QString::fromStdString(name))) !=
        QMessageBox::Yes)
        return;
    (void)agent::removePresetFile(brushDir_, name);
    reloadBrushes();
}

// ── 이미지 메뉴 ───────────────────────────────────────────────────────────

void MainWindow::afterPixelChange() {
    markAutosaveDirty();
    canvas_->invalidateCanvas();
    layerPanel_->refresh();
    thumbTimer_->start();
    refreshTitle();
    refreshStatus();
}

void MainWindow::runAdjust(app::AdjustKind kind) {
    app::Document* doc = activeDocument();
    if (doc == nullptr || canvas_->strokeActive() || canvas_->transformActive()) return;
    const LayerId lid = doc->layers().activeLayer();
    const auto apply = [this, doc, lid](const app::AdjustParams& p) {
        const Result<u32> r = app::adjustLayer(*doc, StrokeSource::humanPen(), lid, p);
        if (!r.ok()) {
            statusBar()->showMessage(QString::fromStdString(r.message()), 4000);
            return false;
        }
        canvas_->invalidateCanvas();
        return r.value() > 0;
    };
    const auto undo = [this, doc] {
        (void)doc->undo();
        canvas_->invalidateCanvas();
    };
    if (kind == app::AdjustKind::Invert || kind == app::AdjustKind::Desaturate) {
        app::AdjustParams p;
        p.kind = kind;
        apply(p);
        afterPixelChange();
        return;
    }
    AdjustDialog dlg(kind, apply, undo, this);
    dlg.exec();
    afterPixelChange();
}

void MainWindow::runCanvasOp(const std::function<Result<void>(app::Document&)>& fn) {
    app::Document* doc = activeDocument();
    if (doc == nullptr || canvas_->strokeActive() || canvas_->transformActive()) return;
    const Result<void> r = fn(*doc);
    if (!r.ok()) {
        statusBar()->showMessage(QString::fromStdString(r.message()), 4000);
        return;
    }
    canvas_->setDocument(doc); // 캔버스 크기가 바뀌었을 수 있다 — 백킹을 새로 잡는다
    canvas_->selectionChangedExternally();
    afterPixelChange();
}

void MainWindow::showImageSizeDialog(bool canvasOnly) {
    app::Document* doc = activeDocument();
    if (doc == nullptr) return;
    const Size cs = doc->canvasSize();
    QDialog dlg(this);
    dlg.setWindowTitle(canvasOnly ? "캔버스 크기" : "이미지 크기");
    auto* form = new QFormLayout(&dlg);
    auto* w = new QSpinBox(&dlg);
    w->setRange(1, 32768);
    w->setValue(cs.width);
    w->setSuffix(" px");
    auto* h = new QSpinBox(&dlg);
    h->setRange(1, 32768);
    h->setValue(cs.height);
    h->setSuffix(" px");
    form->addRow("너비", w);
    form->addRow("높이", h);
    auto* keep = new QCheckBox("비율 유지", &dlg);
    keep->setChecked(!canvasOnly);
    form->addRow("", keep);
    const double ratio = static_cast<double>(cs.width) / std::max(1, cs.height);
    bool syncing = false;
    connect(w, &QSpinBox::valueChanged, &dlg, [&](int v) { if (keep->isChecked() && !syncing) { syncing = true; h->setValue(std::max(1, static_cast<int>(std::lround(v / ratio)))); syncing = false; } });
    connect(h, &QSpinBox::valueChanged, &dlg, [&](int v) { if (keep->isChecked() && !syncing) { syncing = true; w->setValue(std::max(1, static_cast<int>(std::lround(v * ratio)))); syncing = false; } });
    QComboBox* anchor = nullptr;
    if (canvasOnly) {
        anchor = new QComboBox(&dlg);
        anchor->addItems({"왼쪽 위", "위", "오른쪽 위", "왼쪽", "가운데", "오른쪽", "왼쪽 아래", "아래", "오른쪽 아래"});
        anchor->setCurrentIndex(4);
        form->addRow("기준점", anchor);
    }
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    buttons->button(QDialogButtonBox::Ok)->setText("확인");
    buttons->button(QDialogButtonBox::Cancel)->setText("취소");
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() != QDialog::Accepted) return;
    const Size ns{w->value(), h->value()};
    if (ns == cs) return;
    if (canvasOnly) {
        const int a = anchor->currentIndex();
        runCanvasOp([ns, a](app::Document& d) { return app::resizeCanvas(d, StrokeSource::humanPen(), ns, a % 3, a / 3); });
    } else {
        runCanvasOp([ns](app::Document& d) { return app::scaleImage(d, StrokeSource::humanPen(), ns); });
    }
}

void MainWindow::beginFreeTransform() {
    if (activeDocument() == nullptr || canvas_->strokeActive()) return;
    if (canvas_->transformActive()) { canvas_->commitTransform(); return; }
    canvas_->setFocus();
    if (!canvas_->beginTransform()) statusBar()->showMessage("변형할 내용이 없다(빈 레이어·그룹·잠금)", 3000);
    else statusBar()->showMessage("자유 변형: 드래그 이동 · 모서리 확대(Shift 비율) · 바깥 회전(Shift 15°) · Enter 적용 · Esc 취소", 8000);
}

// ── 자동 저장 · 백업 · 복구 ────────────────────────────────────────────────

QString MainWindow::autosaveDir() const {
    const QString dir = QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath("autosave");
    QDir().mkpath(dir);
    return dir;
}

void MainWindow::setupAutosave() {
    autosaveTimer_ = new QTimer(this);
    connect(autosaveTimer_, &QTimer::timeout, this, &MainWindow::autosaveNow);
    const int minutes = QSettings().value("autosave/minutes", 3).toInt();
    if (minutes > 0) autosaveTimer_->start(minutes * 60 * 1000);
}

void MainWindow::markAutosaveDirty() { autosaveDirty_ = true; }

void MainWindow::autosaveNow() {
    app::Document* doc = activeDocument();
    if (doc == nullptr || !autosaveDirty_ || canvas_->strokeActive() || canvas_->transformActive()) return;
    const QString base = QDir(autosaveDir()).filePath(autosaveId_);
    const Result<void> w = doc->writeCopy((base + ".ora").toStdString());
    if (!w.ok()) {
        statusBar()->showMessage("자동 저장 실패: " + QString::fromStdString(w.message()), 5000);
        return;
    }
    QJsonObject meta;
    meta["original"] = QString::fromStdString(doc->fullPath());
    meta["time"] = QDateTime::currentDateTime().toString(Qt::ISODate);
    meta["width"] = doc->canvasSize().width;
    meta["height"] = doc->canvasSize().height;
    QFile f(base + ".json");
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) f.write(QJsonDocument(meta).toJson());
    autosaveDirty_ = false;
    statusBar()->showMessage("자동 저장했다 (" + QDateTime::currentDateTime().toString("HH:mm") + ")", 2000);
}

void MainWindow::clearAutosave() {
    if (autosaveId_.isEmpty()) return;
    const QString base = QDir(autosaveDir()).filePath(autosaveId_);
    QFile::remove(base + ".ora");
    QFile::remove(base + ".json");
    autosaveDirty_ = false;
}

void MainWindow::checkRecovery() {
    QDir dir(autosaveDir());
    const QStringList files = dir.entryList({"*.ora"}, QDir::Files, QDir::Time);
    if (files.isEmpty()) return;
    QDialog dlg(this);
    dlg.setWindowTitle("복구할 문서가 있다");
    dlg.resize(560, 320);
    auto* layout = new QVBoxLayout(&dlg);
    layout->addWidget(new QLabel("지난번에 저장하지 않고 끝난 문서의 자동 저장본이다. 복구하면 '다른 이름으로 저장'으로 저장한다.", &dlg));
    auto* tree = new QTreeWidget(&dlg);
    tree->setHeaderLabels({"원본", "시각", "크기"});
    tree->setRootIsDecorated(false);
    for (const QString& f : files) {
        const QString base = dir.filePath(f.left(f.size() - 4));
        QString original = "(제목 없음)", when, size;
        QFile mf(base + ".json");
        if (mf.open(QIODevice::ReadOnly)) {
            const QJsonObject o = QJsonDocument::fromJson(mf.readAll()).object();
            if (!o["original"].toString().isEmpty()) original = o["original"].toString();
            when = o["time"].toString();
            size = QString("%1×%2").arg(o["width"].toInt()).arg(o["height"].toInt());
        }
        auto* it = new QTreeWidgetItem(tree, {original, when, size});
        it->setData(0, Qt::UserRole, base);
    }
    tree->setCurrentItem(tree->topLevelItem(0));
    layout->addWidget(tree, 1);
    auto* buttons = new QDialogButtonBox(&dlg);
    QPushButton* recover = buttons->addButton("복구", QDialogButtonBox::AcceptRole);
    QPushButton* discard = buttons->addButton("삭제", QDialogButtonBox::DestructiveRole);
    QPushButton* later = buttons->addButton("나중에", QDialogButtonBox::RejectRole);
    layout->addWidget(buttons);
    connect(recover, &QPushButton::clicked, &dlg, &QDialog::accept);
    connect(later, &QPushButton::clicked, &dlg, &QDialog::reject);
    connect(discard, &QPushButton::clicked, &dlg, [&] {
        QTreeWidgetItem* it = tree->currentItem();
        if (it == nullptr) return;
        const QString base = it->data(0, Qt::UserRole).toString();
        QFile::remove(base + ".ora");
        QFile::remove(base + ".json");
        delete it;
        if (tree->topLevelItemCount() == 0) dlg.reject();
    });
    if (dlg.exec() != QDialog::Accepted) return;
    QTreeWidgetItem* it = tree->currentItem();
    if (it == nullptr) return;
    const QString base = it->data(0, Qt::UserRole).toString();
    // 시작 직후의 빈 새 문서(경로 없음·실행취소 항목 0)는 묻지 않고 버린다.
    if (app::Document* cur = activeDocument();
        !(cur != nullptr && cur->fullPath().empty() && cur->undoStack().undoCount() == 0) && !confirmDiscard())
        return;
    if (app::Document* old = activeDocument()) {
        canvas_->setDocument(nullptr);
        layerPanel_->setDocument(nullptr);
        (void)app_.closeDocument(old, false);
    }
    Result<app::IDocumentBridge*> opened = app_.open((base + ".ora").toStdString());
    if (!opened.ok()) {
        QMessageBox::warning(this, "복구", QString::fromStdString(opened.message()));
        attachDocument(activeDocument());
        return;
    }
    attachDocument(static_cast<app::Document*>(opened.value()));
    recovered_ = true;
    // 복구본은 지우지 않고 이 세션의 자동 저장본으로 이어받는다 — 다음 자동 저장 전에 또 죽어도 남는다.
    // 사용자가 제대로 저장하거나 닫으면 clearAutosave() 가 치운다.
    const QString mine = QDir(autosaveDir()).filePath(autosaveId_);
    if (!QFile::rename(base + ".ora", mine + ".ora")) QFile::remove(base + ".ora");
    if (!QFile::rename(base + ".json", mine + ".json")) QFile::remove(base + ".json");
    markAutosaveDirty();
    statusBar()->showMessage("자동 저장본에서 복구했다 — Ctrl+S 로 저장 위치를 정해라", 8000);
}

void MainWindow::showAutosaveSettings() {
    QDialog dlg(this);
    dlg.setWindowTitle("자동 저장 · 백업");
    auto* form = new QFormLayout(&dlg);
    auto* minutes = new QSpinBox(&dlg);
    minutes->setRange(0, 60);
    minutes->setSpecialValueText("끔");
    minutes->setSuffix(" 분");
    minutes->setValue(QSettings().value("autosave/minutes", 3).toInt());
    form->addRow("자동 저장 간격", minutes);
    auto* backup = new QCheckBox("저장할 때 원본을 .bak 로 남긴다", &dlg);
    backup->setChecked(QSettings().value("backup/enabled", true).toBool());
    form->addRow("", backup);
    form->addRow(new QLabel("자동 저장 위치: " + autosaveDir(), &dlg));
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    buttons->button(QDialogButtonBox::Ok)->setText("확인");
    buttons->button(QDialogButtonBox::Cancel)->setText("취소");
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() != QDialog::Accepted) return;
    QSettings s;
    s.setValue("autosave/minutes", minutes->value());
    s.setValue("backup/enabled", backup->isChecked());
    autosaveTimer_->stop();
    if (minutes->value() > 0) autosaveTimer_->start(minutes->value() * 60 * 1000);
}

void MainWindow::togglePanels() {
    panelsHidden_ = !panelsHidden_;
    const bool show = !panelsHidden_;
    colorDock_->setVisible(show);
    layerDock_->setVisible(show);
    navDock_->setVisible(show);
    brushDock_->setVisible(show);
    historyDock_->setVisible(show);
    paletteDock_->setVisible(show);
    refDock_->setVisible(show);
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
    case Tool::Line: toolName = "직선"; toolIcon = "line"; break;
    case Tool::Rectangle: toolName = "사각형"; toolIcon = "rectangle"; break;
    case Tool::Ellipse: toolName = "타원"; toolIcon = "circle"; break;
    case Tool::Gradient: toolName = "그라데이션"; toolIcon = "gradient"; break;
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
        title = (recovered_ ? QString("복구된 문서 (저장 위치 미정)") : p.empty() ? QString("제목 없음") : QString::fromStdString(p)) +
                (doc->isSaved() && !recovered_ ? "" : " *") + " — Mari Paint";
    }
    setWindowTitle(title);
    refreshStatus();
    if (historyPanel_ != nullptr) historyPanel_->refresh();
}

} // namespace mari::ui
