// Mari Paint — 단축키 편집기 (편집 › 단축키 설정…)
//
// 기본값은 포토샵과 같다(docs/09 단축키 표). 사용자 변경은 QSettings `shortcuts/<id>` 에 저장하고,
// 기본값과 같아지면 키를 지운다. id = "<메뉴>/<항목 텍스트>" — 텍스트는 & 와 뒤에 붙인 키 힌트를 뗀 것.
#ifndef MARI_UI_SHORTCUT_DIALOG_HPP
#define MARI_UI_SHORTCUT_DIALOG_HPP

#include <QDialog>
#include <QKeySequence>
#include <QList>
#include <QString>

#include <vector>

class QAction;
class QKeySequenceEdit;
class QLabel;
class QMenuBar;
class QToolBar;
class QTreeWidget;
class QTreeWidgetItem;

namespace mari::ui {

/// 창의 모든 단축키 동작을 한곳에 모은다. 등록 시점의 단축키가 "기본값"이다.
class ShortcutRegistry final {
public:
    struct Entry {
        QString id;
        QString group;
        QString name;
        QAction* action = nullptr;
        QList<QKeySequence> defaults;
    };

    void add(const QString& group, QAction* action);
    /// 메뉴바의 모든 메뉴(부메뉴 포함)와 툴바 동작을 등록하고 저장된 변경을 입힌다.
    void collect(QMenuBar* bar, const std::vector<std::pair<QString, QToolBar*>>& toolbars);

    [[nodiscard]] const std::vector<Entry>& entries() const noexcept { return entries_; }
    [[nodiscard]] const Entry* find(const QString& id) const;
    /// 단축키를 바꾸고 저장한다. 같은 키를 쓰던 다른 동작은 비운다(포토샵과 같은 정책). 그 id 들을 돌려준다.
    QStringList assign(const QString& id, const QList<QKeySequence>& seqs);
    void resetAll();
    [[nodiscard]] static QString cleanName(QString text);

private:
    void applySaved(Entry& e);
    static void save(const Entry& e);

    std::vector<Entry> entries_;
};

class ShortcutDialog final : public QDialog {
    Q_OBJECT
public:
    explicit ShortcutDialog(ShortcutRegistry& reg, QWidget* parent = nullptr);

private:
    void rebuild();
    void onCurrentChanged();
    void assignFromEditor();
    void clearCurrent();
    void resetCurrent();
    [[nodiscard]] QString currentId() const;

    ShortcutRegistry& reg_;
    QTreeWidget* tree_ = nullptr;
    QKeySequenceEdit* edit_ = nullptr;
    QLabel* note_ = nullptr;
};

} // namespace mari::ui

#endif // MARI_UI_SHORTCUT_DIALOG_HPP
