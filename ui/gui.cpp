// Mari Paint — GUI 진입점 구현 (ui/gui.hpp)
#include "gui.hpp"

#include "main_window.hpp"

#include <mari/app/application.hpp>
#include <mari/record/sigan_recorder.hpp>

#include <QApplication>
#include <QPalette>
#include <QDir>
#include <QStandardPaths>
#include <QStatusBar>

#include <cstdio>
#include <exception>
#include <memory>


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

int runGui(int argc, char** argv) {
    // 잡히지 않은 예외는 조용히 죽지 않고 이유를 stderr 에 남긴다(0xc0000409 는 아무것도 안 알려 준다).
    std::set_terminate([] {
        try {
            if (const std::exception_ptr e = std::current_exception()) {
                std::rethrow_exception(e);
            }
            std::fputs("mari-paint: terminate() - no exception\n", stderr);
        } catch (const std::exception& ex) {
            std::fprintf(stderr, "mari-paint: uncaught exception: %s\n", ex.what());
        } catch (...) {
            std::fputs("mari-paint: uncaught exception (unknown type)\n", stderr);
        }
        std::fflush(stderr);
        std::abort();
    });
    // Qt 는 stderr 가 콘솔이 아니면(리다이렉트) 메시지를 OutputDebugString 으로 보내 버린다.
    // 진단은 파일로 받을 수 있어야 한다 — 콘솔 강제.
    if (qEnvironmentVariableIsEmpty("QT_LOGGING_TO_CONSOLE")) {
        qputenv("QT_LOGGING_TO_CONSOLE", "1");
    }
    const bool trace = qEnvironmentVariableIsSet("MARI_GUI_TRACE");
    const auto tr = [trace](const char* what) {
        if (trace) {
            std::fprintf(stderr, "[mari-gui] %s\n", what);
            std::fflush(stderr);
        }
    };
    tr("start");
    QApplication qapp(argc, argv);
    tr("QApplication ok"); heapCheck("after QApplication");
    // 다크 테마: Fusion + 팔레트(Qt 6.5+ 권장). 페인팅 앱이 중간 회색 UI 를 쓰는 이유는
    // 색 지각 중립성이다 — 밝은 크롬 옆에서는 캔버스 색이 달라 보인다.
    QApplication::setStyle("Fusion");
    {
        QPalette pal;
        const QColor window(43, 43, 43), base(30, 30, 30), text(220, 220, 220), hl(61, 123, 217);
        pal.setColor(QPalette::Window, window);
        pal.setColor(QPalette::WindowText, text);
        pal.setColor(QPalette::Base, base);
        pal.setColor(QPalette::AlternateBase, window);
        pal.setColor(QPalette::ToolTipBase, base);
        pal.setColor(QPalette::ToolTipText, text);
        pal.setColor(QPalette::Text, text);
        pal.setColor(QPalette::Button, window);
        pal.setColor(QPalette::ButtonText, text);
        pal.setColor(QPalette::BrightText, Qt::red);
        pal.setColor(QPalette::Highlight, hl);
        pal.setColor(QPalette::HighlightedText, Qt::white);
        pal.setColor(QPalette::PlaceholderText, QColor(120, 120, 120));
        pal.setColor(QPalette::Disabled, QPalette::Text, QColor(110, 110, 110));
        pal.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(110, 110, 110));
        pal.setColor(QPalette::Disabled, QPalette::WindowText, QColor(110, 110, 110));
        QApplication::setPalette(pal);
    }
    QApplication::setApplicationName("Mari Paint");
    QApplication::setOrganizationName("Mari");

    // 🔴 기록 배선(docs/06). CLI 와 같은 공장을 같은 순서로 꽂는다 — 공장이 앱보다 오래 산다.
    //    사람 획도 여기서 열린 저널에 남는다. 저널은 로컬 앱 데이터 아래에 둔다.
    //    Sigan 파이프(싱크)는 붙이지 않는다 — 정본은 저널이고 그것만으로 완전하다(docs/03 4.2).
    const QString journalDir =
        QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath("journal");
    QDir().mkpath(journalDir);

    record::RecordingConfig rc;
    rc.journalDir = journalDir.toStdString();
    auto recorders = std::make_unique<record::SiganRecorderFactory>(std::move(rc));
    tr("recorder factory ok"); heapCheck("after factory");

    app::Application app;
    heapCheck("after Application ctor");
    if (!qEnvironmentVariableIsSet("MARI_GUI_NOREC")) { // 진단용 토글
        app.setRecorderFactory(recorders.get());
    }
    app.setVisible(true);

    // 빈 캔버스로 시작한다. 실패하면(저널을 못 열었다 등) 문서 없이 뜨고 창이 이유를 보여 준다.
    QString startupError;
    if (!qEnvironmentVariableIsSet("MARI_GUI_NODOC")) { // 진단용 토글
        const Result<app::IDocumentBridge*> first = app.createDocument(1920, 1080);
        if (!first.ok()) {
            startupError = QString::fromStdString(first.message());
        }
    }
    tr(startupError.isEmpty() ? "document ok" : "document FAILED"); heapCheck("after createDocument");

    MainWindow win(app, journalDir);
    tr("MainWindow ok"); heapCheck("after MainWindow ctor");
    win.show();
    tr("shown — entering event loop");
    if (!startupError.isEmpty()) {
        win.statusBar()->showMessage("문서를 만들지 못했다: " + startupError);
    }
    const int rc_ = QApplication::exec();
    tr("event loop done");
    // 창이 먼저 죽고, 문서(=기록 구간)가 닫히고, 그다음 공장이 닫힌다.
    return rc_;
}

} // namespace mari::ui
