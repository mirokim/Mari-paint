// Mari Paint — 최소 UI (docs/08 3.4): 툴바 · 레이어 패널 · 캔버스. 그 이상은 나중에.
//
// 창 방식은 docs/08 8절에서 미결이었다. 여기서는 **단일 창 + 도킹 패널**로 시작한다 —
// 플로팅은 창이 여러 개라 콜드 스타트와 포커스 관리가 무겁고, 도킹은 Qt 가 공짜로 준다.
// 바꾸고 싶으면 QDockWidget 을 떼면 된다. 캔버스는 이 결정을 모른다.
#ifndef MARI_UI_MAIN_WINDOW_HPP
#define MARI_UI_MAIN_WINDOW_HPP

#include <mari/app/application.hpp>
#include <mari/app/live_stroke.hpp>
#include <mari/brush/preset.hpp>

#include <QMainWindow>

#include <vector>

class QAction;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QToolButton;

namespace mari::ui {

class CanvasWidget;

class MainWindow final : public QMainWindow {
    Q_OBJECT
public:
    /// `app` 은 소유하지 않는다. 창보다 오래 살아야 한다.
    /// `journalPath` 는 상태 표시줄에 보여 줄 기록 위치. 비면 "기록 없음".
    explicit MainWindow(app::Application& app, QString journalPath, QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent* e) override;

private:
    void buildMenus();
    void buildToolbar();
    void buildLayerPanel();
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
    void addLayer();
    void removeLayer();
    void refreshLayerList();
    void onLayerRowChanged(int row);
    void onLayerItemChanged(QListWidgetItem* item);
    void pickColor();
    void refreshStatus();
    void refreshTitle();

    app::Application& app_;
    QString journalPath_;
    CanvasWidget* canvas_ = nullptr;

    std::vector<brush::MariBrushPreset> brushes_;
    QComboBox* brushCombo_ = nullptr;
    QDoubleSpinBox* sizeSpin_ = nullptr;
    QComboBox* smoothingCombo_ = nullptr;
    QToolButton* colorButton_ = nullptr;
    QAction* eraserAction_ = nullptr;
    QAction* undoAction_ = nullptr;
    QAction* redoAction_ = nullptr;
    QColor color_{0, 0, 0};

    QListWidget* layerList_ = nullptr;
    bool layerListBusy_ = false;

    QLabel* statusInput_ = nullptr;
    QLabel* statusRecord_ = nullptr;
    QLabel* statusLatency_ = nullptr;
    QLabel* statusStrokes_ = nullptr;
    QLabel* statusView_ = nullptr;
    u64 strokes_ = 0;
    u64 recordedStrokes_ = 0;
    u64 rolledBack_ = 0;
};

} // namespace mari::ui

#endif // MARI_UI_MAIN_WINDOW_HPP
