// Mari Paint — 최소 UI (docs/08 3.4): 도구 툴바 · 옵션 툴바 · 색/레이어 도크 · 캔버스
//
// 창 방식은 docs/08 8절에서 미결이었다. 여기서는 **단일 창 + 도킹 패널**로 시작한다 —
// 플로팅은 창이 여러 개라 콜드 스타트와 포커스 관리가 무겁고, 도킹은 Qt 가 공짜로 준다.
// 레이아웃은 페인팅 앱 공통분모다(Krita·CSP·SAI): 도구(좌) · 캔버스(중) · 색+레이어(우).
// 단축키는 docs/09-ui-research.md 의 표를 따른다.
#ifndef MARI_UI_MAIN_WINDOW_HPP
#define MARI_UI_MAIN_WINDOW_HPP

#include <mari/app/application.hpp>
#include <mari/app/image_ops.hpp>
#include <mari/app/live_stroke.hpp>
#include <mari/brush/preset.hpp>

#include <QColor>
#include <QMainWindow>

#include <functional>
#include <memory>
#include <vector>

class QAction;
class QActionGroup;
class QComboBox;
class QDockWidget;
class QDoubleSpinBox;
class QLabel;
class QSlider;
class QSpinBox;
class QToolBar;
class QTimer;

namespace mari::ui {

class CanvasWidget;
class ColorPanel;
class LayerPanel;
class Navigator;
class PopupPalette;
class BrushPanel;
class HistoryPanel;
class PalettePanel;
class ReferencePanel;
class ShortcutRegistry;
enum class Tool;

class MainWindow final : public QMainWindow {
    Q_OBJECT
public:
    /// `app` 은 소유하지 않는다. 창보다 오래 살아야 한다.
    /// `journalPath` 는 상태 표시줄(디버그)에 보여 줄 기록 위치.
    explicit MainWindow(app::Application& app, QString journalPath, QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent* e) override;

private:
    void buildMenus();
    void buildToolbars();
    void buildDocks();
    void buildStatusBar();

    [[nodiscard]] app::LiveStrokeConfig strokeConfig() const;
    [[nodiscard]] app::Document* activeDocument() const;
    void attachDocument(app::Document* doc);

    void newDocument();
    void openDocument();
    bool saveDocument(bool forceDialog);
    bool confirmDiscard();

    void undo();
    void redo();
    void setBrushSize(f64 px);
    void stepBrushSize(int direction);
    void stepOpacity(int direction);
    void togglePanels();
    void toggleCanvasOnly();
    void showTabletDialog();
    void showPalette(const QPoint& globalPos);
    void mergeDown();
    void flattenImage();
    // 이미지 메뉴
    void runAdjust(app::AdjustKind kind);
    void runCanvasOp(const std::function<Result<void>(app::Document&)>& fn);
    void showImageSizeDialog(bool canvasOnly);
    void beginFreeTransform();
    void afterPixelChange();
    // 자동 저장 · 백업 · 복구
    void setupAutosave();
    void autosaveNow();
    void clearAutosave();
    void markAutosaveDirty();
    void checkRecovery();
    void showAutosaveSettings();
    [[nodiscard]] QString autosaveDir() const;
    void showShortcutDialog();
    // 브러시 라이브러리(.mbp 폴더 = 헤드리스와 공유)
    void reloadBrushes(const QString& selectName = QString());
    void importBrushes();
    void editBrush(int index, bool forceCopy);
    void duplicateBrush(int index);
    void deleteBrush(int index);
    void showImportReport(const QString& title, const std::vector<std::string>& notes,
                          const std::vector<std::string>& importedNames);
    void refreshStatus();
    void refreshTitle();

    app::Application& app_;
    QString journalPath_;
    CanvasWidget* canvas_ = nullptr;
    ColorPanel* colorPanel_ = nullptr;
    LayerPanel* layerPanel_ = nullptr;
    QDockWidget* colorDock_ = nullptr;
    QDockWidget* layerDock_ = nullptr;
    QDockWidget* navDock_ = nullptr;
    QDockWidget* brushDock_ = nullptr;
    QDockWidget* historyDock_ = nullptr;
    QDockWidget* paletteDock_ = nullptr;
    QDockWidget* refDock_ = nullptr;
    HistoryPanel* historyPanel_ = nullptr;
    PalettePanel* palettePanel_ = nullptr;
    ReferencePanel* refPanel_ = nullptr;
    BrushPanel* brushPanel_ = nullptr;
    usize builtinBrushCount_ = 0;
    std::string brushDir_;
    Navigator* navigator_ = nullptr;
    PopupPalette* palette_ = nullptr;
    bool canvasOnly_ = false;
    QByteArray savedLayoutState_;
    QToolBar* toolsBar_ = nullptr;
    QToolBar* optionsBar_ = nullptr;
    bool panelsHidden_ = false;

    std::vector<brush::MariBrushPreset> brushes_;
    QComboBox* brushCombo_ = nullptr;
    QSlider* sizeSlider_ = nullptr;
    QDoubleSpinBox* sizeSpin_ = nullptr;
    QSlider* opacitySlider_ = nullptr;
    QSpinBox* opacitySpin_ = nullptr;
    QComboBox* smoothingCombo_ = nullptr;
    QSpinBox* deadZoneSpin_ = nullptr;
    QWidget* floodOptions_ = nullptr;
    QWidget* transformOptions_ = nullptr;
    QDoubleSpinBox* xfDx_ = nullptr;
    QDoubleSpinBox* xfDy_ = nullptr;
    QDoubleSpinBox* xfSx_ = nullptr;
    QDoubleSpinBox* xfSy_ = nullptr;
    QDoubleSpinBox* xfRot_ = nullptr;
    bool xfSyncing_ = false;
    QSpinBox* toleranceSpin_ = nullptr;
    QSpinBox* gapSpin_ = nullptr;
    QActionGroup* toolGroup_ = nullptr;
    QAction* undoAction_ = nullptr;
    QAction* redoAction_ = nullptr;
    QAction* debugStatusAction_ = nullptr;
    struct ViewActions {
        QAction* zoomIn = nullptr;
        QAction* zoomOut = nullptr;
        QAction* zoomReset = nullptr;
        QAction* fit = nullptr;
        QAction* rotL = nullptr;
        QAction* rotR = nullptr;
        QAction* mirror = nullptr;
        QAction* panels = nullptr;
    } viewActions_;
    bool syncingSize_ = false;
    QTimer* thumbTimer_ = nullptr;
    QTimer* autosaveTimer_ = nullptr;
    QString autosaveId_;
    bool autosaveDirty_ = false;
    bool recovered_ = false; ///< 자동 저장본에서 열었다 — 저장은 반드시 '다른 이름으로'
    std::unique_ptr<ShortcutRegistry> shortcuts_;

    QLabel* statusToolIcon_ = nullptr;
    QString statusToolIconName_;
    QLabel* statusMain_ = nullptr;
    QLabel* statusDebug_ = nullptr;
    QLabel* statusView_ = nullptr;
    u64 strokes_ = 0;
    u64 recordedStrokes_ = 0;
    u64 rolledBack_ = 0;
};

} // namespace mari::ui

#endif // MARI_UI_MAIN_WINDOW_HPP
