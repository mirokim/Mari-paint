// Mari Paint — 단축키 편집기 구현 (ui/shortcut_dialog.hpp)
#include "shortcut_dialog.hpp"

#include <QAction>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QToolBar>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace mari::ui {

namespace {
constexpr int kIdRole = Qt::UserRole;

QString joinSeqs(const QList<QKeySequence>& seqs) {
    QStringList parts;
    for (const QKeySequence& s : seqs) parts << s.toString(QKeySequence::NativeText);
    return parts.join(", ");
}

/// "shortcuts/<id>" 는 '/' 가 그룹 구분자라 id 를 그대로 못 쓴다 — 치환한다.
QString settingsKey(const QString& id) {
    QString k = id;
    k.replace('/', "|");
    return "shortcuts/" + k;
}
} // namespace

QString ShortcutRegistry::cleanName(QString text) {
    text.remove('&');
    // "파일(F)" 처럼 니모닉 괄호는 뗀다.
    static const QRegularExpression mnemonic(R"(\s*\([A-Za-z]\)$)");
    text.remove(mnemonic);
    // "붓 크게  ]" 처럼 텍스트 뒤에 붙였던 키 힌트(두 칸 공백 뒤)는 뗀다.
    const int hint = text.indexOf("  ");
    if (hint > 0) text.truncate(hint);
    if (text.endsWith("...")) text.chop(3);
    return text.trimmed();
}

void ShortcutRegistry::add(const QString& group, QAction* action) {
    if (action == nullptr || action->isSeparator() || action->text().isEmpty()) return;
    if (action->menu() != nullptr) return;
    if (!action->isEnabled() && action->shortcuts().isEmpty()) return; // 안내용 비활성 항목
    Entry e;
    e.group = group;
    e.name = cleanName(action->text());
    e.id = group + "/" + e.name;
    e.action = action;
    e.defaults = action->shortcuts();
    for (const Entry& had : entries_) {
        if (had.id == e.id) return;
    }
    applySaved(e);
    entries_.push_back(std::move(e));
}

void ShortcutRegistry::collect(QMenuBar* bar, const std::vector<std::pair<QString, QToolBar*>>& toolbars) {
    for (QAction* top : bar->actions()) {
        QMenu* menu = top->menu();
        if (menu == nullptr) continue;
        const QString group = cleanName(top->text());
        for (QAction* a : menu->actions()) {
            if (QMenu* sub = a->menu()) {
                for (QAction* b : sub->actions()) add(group + " › " + cleanName(a->text()), b);
            } else {
                add(group, a);
            }
        }
    }
    for (const auto& [name, tb] : toolbars) {
        for (QAction* a : tb->actions()) add(name, a);
    }
}

const ShortcutRegistry::Entry* ShortcutRegistry::find(const QString& id) const {
    for (const Entry& e : entries_) {
        if (e.id == id) return &e;
    }
    return nullptr;
}

void ShortcutRegistry::applySaved(Entry& e) {
    QSettings s;
    const QString key = settingsKey(e.id);
    if (!s.contains(key)) return;
    QList<QKeySequence> seqs;
    for (const QString& part : s.value(key).toString().split(';', Qt::SkipEmptyParts)) {
        seqs << QKeySequence::fromString(part, QKeySequence::PortableText);
    }
    e.action->setShortcuts(seqs);
}

void ShortcutRegistry::save(const Entry& e) {
    QSettings s;
    const QString key = settingsKey(e.id);
    if (e.action->shortcuts() == e.defaults) {
        s.remove(key);
        return;
    }
    QStringList parts;
    for (const QKeySequence& k : e.action->shortcuts()) parts << k.toString(QKeySequence::PortableText);
    s.setValue(key, parts.join(';'));
}

QStringList ShortcutRegistry::assign(const QString& id, const QList<QKeySequence>& seqs) {
    QStringList bumped;
    Entry* target = nullptr;
    for (Entry& e : entries_) {
        if (e.id == id) target = &e;
    }
    if (target == nullptr) return bumped;
    for (Entry& e : entries_) {
        if (&e == target) continue;
        QList<QKeySequence> kept;
        bool changed = false;
        for (const QKeySequence& k : e.action->shortcuts()) {
            if (!k.isEmpty() && seqs.contains(k)) {
                changed = true;
            } else {
                kept << k;
            }
        }
        if (changed) {
            e.action->setShortcuts(kept);
            save(e);
            bumped << e.id;
        }
    }
    target->action->setShortcuts(seqs);
    save(*target);
    return bumped;
}

void ShortcutRegistry::resetAll() {
    for (Entry& e : entries_) {
        e.action->setShortcuts(e.defaults);
        save(e);
    }
}

// ── 대화상자 ──────────────────────────────────────────────────────────────

ShortcutDialog::ShortcutDialog(ShortcutRegistry& reg, QWidget* parent) : QDialog(parent), reg_(reg) {
    setWindowTitle("단축키 설정");
    resize(620, 560);
    auto* layout = new QVBoxLayout(this);

    auto* filter = new QLineEdit(this);
    filter->setPlaceholderText("찾기…");
    filter->setClearButtonEnabled(true);
    layout->addWidget(filter);

    tree_ = new QTreeWidget(this);
    tree_->setColumnCount(2);
    tree_->setHeaderLabels({"동작", "단축키"});
    tree_->header()->setStretchLastSection(false);
    tree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tree_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    tree_->setRootIsDecorated(true);
    layout->addWidget(tree_, 1);

    auto* row = new QHBoxLayout();
    row->addWidget(new QLabel("새 단축키", this));
    edit_ = new QKeySequenceEdit(this);
    edit_->setClearButtonEnabled(true);
    row->addWidget(edit_, 1);
    auto* assign = new QPushButton("지정", this);
    auto* clear = new QPushButton("비우기", this);
    auto* reset = new QPushButton("기본값", this);
    row->addWidget(assign);
    row->addWidget(clear);
    row->addWidget(reset);
    layout->addLayout(row);

    note_ = new QLabel(this);
    note_->setWordWrap(true);
    note_->setText("기본값은 포토샵과 같다. 이미 쓰는 키를 지정하면 그 동작의 키는 비워진다.");
    layout->addWidget(note_);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close | QDialogButtonBox::RestoreDefaults, this);
    buttons->button(QDialogButtonBox::Close)->setText("닫기");
    buttons->button(QDialogButtonBox::RestoreDefaults)->setText("전부 기본값");
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons->button(QDialogButtonBox::RestoreDefaults), &QPushButton::clicked, this, [this] {
        reg_.resetAll();
        rebuild();
        note_->setText("전부 기본값(포토샵)으로 되돌렸다.");
    });
    connect(assign, &QPushButton::clicked, this, &ShortcutDialog::assignFromEditor);
    connect(edit_, &QKeySequenceEdit::editingFinished, this, [this] {
        // 한 조합만 받는다(연타 시퀀스는 페인팅에 쓸모없다).
        const QKeySequence k = edit_->keySequence();
        if (k.count() > 1) edit_->setKeySequence(QKeySequence(k[0]));
    });
    connect(clear, &QPushButton::clicked, this, &ShortcutDialog::clearCurrent);
    connect(reset, &QPushButton::clicked, this, &ShortcutDialog::resetCurrent);
    connect(tree_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem*, QTreeWidgetItem*) { onCurrentChanged(); });
    connect(filter, &QLineEdit::textChanged, this, [this](const QString& t) {
        for (int g = 0; g < tree_->topLevelItemCount(); ++g) {
            QTreeWidgetItem* group = tree_->topLevelItem(g);
            int shown = 0;
            for (int i = 0; i < group->childCount(); ++i) {
                QTreeWidgetItem* it = group->child(i);
                const bool hit = t.isEmpty() || it->text(0).contains(t, Qt::CaseInsensitive) ||
                                 it->text(1).contains(t, Qt::CaseInsensitive);
                it->setHidden(!hit);
                if (hit) ++shown;
            }
            group->setHidden(shown == 0);
        }
    });
    rebuild();
}

void ShortcutDialog::rebuild() {
    const QString keep = currentId();
    tree_->clear();
    QTreeWidgetItem* current = nullptr;
    for (const ShortcutRegistry::Entry& e : reg_.entries()) {
        QTreeWidgetItem* group = nullptr;
        for (int g = 0; g < tree_->topLevelItemCount(); ++g) {
            if (tree_->topLevelItem(g)->text(0) == e.group) group = tree_->topLevelItem(g);
        }
        if (group == nullptr) {
            group = new QTreeWidgetItem(tree_, {e.group});
            group->setFlags(Qt::ItemIsEnabled);
            group->setExpanded(true);
        }
        auto* it = new QTreeWidgetItem(group, {e.name, joinSeqs(e.action->shortcuts())});
        it->setData(0, kIdRole, e.id);
        if (e.action->shortcuts() != e.defaults) {
            QFont f = it->font(0);
            f.setBold(true);
            it->setFont(0, f);
            it->setFont(1, f);
            it->setToolTip(1, "기본값: " + joinSeqs(e.defaults));
        }
        if (e.id == keep) current = it;
    }
    if (current != nullptr) tree_->setCurrentItem(current);
}

QString ShortcutDialog::currentId() const {
    const QTreeWidgetItem* it = tree_->currentItem();
    return it == nullptr ? QString() : it->data(0, kIdRole).toString();
}

void ShortcutDialog::onCurrentChanged() {
    const ShortcutRegistry::Entry* e = reg_.find(currentId());
    edit_->setEnabled(e != nullptr);
    if (e == nullptr) {
        edit_->clear();
        return;
    }
    edit_->setKeySequence(e->action->shortcuts().isEmpty() ? QKeySequence() : e->action->shortcuts().first());
    edit_->setFocus();
}

void ShortcutDialog::assignFromEditor() {
    const QString id = currentId();
    const ShortcutRegistry::Entry* e = reg_.find(id);
    if (e == nullptr) return;
    QList<QKeySequence> seqs;
    if (!edit_->keySequence().isEmpty()) seqs << edit_->keySequence();
    const QStringList bumped = reg_.assign(id, seqs);
    rebuild();
    note_->setText(bumped.isEmpty() ? QString("'%1' ← %2").arg(e->name, joinSeqs(seqs))
                                    : QString("'%1' ← %2 · 비워진 동작: %3").arg(e->name, joinSeqs(seqs), bumped.join(", ")));
}

void ShortcutDialog::clearCurrent() {
    const QString id = currentId();
    if (reg_.find(id) == nullptr) return;
    reg_.assign(id, {});
    rebuild();
}

void ShortcutDialog::resetCurrent() {
    const QString id = currentId();
    const ShortcutRegistry::Entry* e = reg_.find(id);
    if (e == nullptr) return;
    reg_.assign(id, e->defaults);
    rebuild();
}

} // namespace mari::ui
