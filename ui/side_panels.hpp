// Mari Paint — 작은 도크 세 개: 실행취소 히스토리 · 팔레트 · 참조 이미지
#ifndef MARI_UI_SIDE_PANELS_HPP
#define MARI_UI_SIDE_PANELS_HPP

#include <mari/app/document.hpp>

#include <QColor>
#include <QImage>
#include <QWidget>

#include <vector>

class QListWidget;
class QLabel;

namespace mari::ui {

/// 실행취소 히스토리: 과거(적용됨) · 현재 · 미래(다시 실행 가능, 흐리게). 항목 클릭 = 거기까지 되돌리기/다시 하기.
class HistoryPanel final : public QWidget {
    Q_OBJECT
public:
    explicit HistoryPanel(QWidget* parent = nullptr);
    void setDocument(app::Document* doc);
    /// 스택이 바뀔 때마다 부른다(획·연산·실행취소 뒤).
    void refresh();
signals:
    /// n>0: n 단계 되돌리기, n<0: -n 단계 다시 하기. MainWindow 가 문서에 적용하고 화면을 갱신한다.
    void jumpRequested(int steps);
private:
    app::Document* doc_ = nullptr;
    QListWidget* list_ = nullptr;
    bool busy_ = false;
};

/// 팔레트: 견본 격자. 클릭 = 전경색, 우클릭 = 삭제, [+] = 현재 전경색 추가. QSettings 에 저장.
class PalettePanel final : public QWidget {
    Q_OBJECT
public:
    explicit PalettePanel(QWidget* parent = nullptr);
    void setCurrentColor(const QColor& c) { current_ = c; }
    void addColor(const QColor& c);
signals:
    void colorChosen(const QColor& c);
private:
    void rebuild();
    void save();
    std::vector<QColor> colors_;
    QColor current_ = Qt::black;
    QListWidget* list_ = nullptr;
};

/// 참조 이미지: 파일을 열어 놓고 휠 줌·드래그 팬. Alt+클릭(또는 스포이드 도구 상태 무관하게 우클릭)으로 색을 딴다.
class ReferencePanel final : public QWidget {
    Q_OBJECT
public:
    explicit ReferencePanel(QWidget* parent = nullptr);
    void open();
    void clear();
signals:
    void colorPicked(const QColor& c);
protected:
    void paintEvent(QPaintEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
private:
    void fit();
    QImage img_;
    double zoom_ = 1.0;
    QPointF offset_;   ///< 이미지 원점의 위젯 좌표
    QPointF dragLast_;
    bool dragging_ = false;
    bool needFit_ = true;
};

} // namespace mari::ui

#endif // MARI_UI_SIDE_PANELS_HPP
