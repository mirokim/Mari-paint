// Mari Paint — 새 문서 대화상자 (포토샵 · CSP 꼴: 템플릿 목록 + 크기 + 배경)
//
// 시작할 때(랜딩)와 파일 → 새 문서 둘 다 이걸 띄운다. 마지막으로 쓴 값은 QSettings "newdoc/*" 에
// 남겨 다음에 그대로 뜬다. 배경 기본은 **흰색** — 투명 캔버스는 처음 켠 사람을 당황시킨다.
#ifndef MARI_UI_NEW_DOCUMENT_DIALOG_HPP
#define MARI_UI_NEW_DOCUMENT_DIALOG_HPP

#include <mari/core/types.hpp>

#include <QColor>
#include <QDialog>

#include <optional>

class QLabel;
class QListWidget;
class QRadioButton;
class QSpinBox;
class QToolButton;

namespace mari::ui {

class NewDocumentDialog final : public QDialog {
    Q_OBJECT
public:
    /// landing = 시작 화면으로 떴다(제목·버튼 문구만 다르다).
    explicit NewDocumentDialog(QWidget* parent = nullptr, bool landing = false);

    [[nodiscard]] int canvasWidth() const;
    [[nodiscard]] int canvasHeight() const;
    /// 없으면 투명 캔버스.
    [[nodiscard]] std::optional<Color8> background() const;

    struct Template {
        const char* name;
        int w, h;
        const char* note;
    };

protected:
    void resizeEvent(QResizeEvent* e) override;

private:
    void applyTemplate(int row);
    void syncTemplateSelection();
    void updatePreview();
    void pickCustomColor();

    QListWidget* list_ = nullptr;
    QSpinBox* width_ = nullptr;
    QSpinBox* height_ = nullptr;
    QRadioButton* bgWhite_ = nullptr;
    QRadioButton* bgTransparent_ = nullptr;
    QRadioButton* bgCustom_ = nullptr;
    QToolButton* customSwatch_ = nullptr;
    QLabel* preview_ = nullptr;
    QLabel* info_ = nullptr;
    QColor custom_{0xf0, 0xe6, 0xd2};
    bool syncing_ = false;
};

} // namespace mari::ui

#endif // MARI_UI_NEW_DOCUMENT_DIALOG_HPP
