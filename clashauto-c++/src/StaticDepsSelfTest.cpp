#include "StaticDepsSelfTest.h"

#include <QCoreApplication>
#include <QImageReader>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QSqlDatabase>
#include <QSslSocket>
#include <QStringList>
#include <QtGlobal>

#include <cstdio>

namespace {

int g_fail = 0;

void check(bool ok, const char *what, const QString &detail = QString())
{
    std::fprintf(stdout, "  [%s] %s%s\n", ok ? "ok" : "FAIL", what,
                 detail.isEmpty() ? "" : qPrintable(QStringLiteral("  (") + detail + QLatin1Char(')')));
    if (!ok)
        ++g_fail;
}

} // namespace

int runStaticDepsSelfTest()
{
    std::fprintf(stdout, "STATICDEPS-SELFTEST: Qt %s\n", qVersion());

    // —— 平台插件 ——
    // 走到这里说明 QApplication 已经构造成功；平台插件缺失时 Qt 会在构造时就 qFatal，
    // 根本到不了这一行。所以这条只是把结论记进日志，真正的门禁是"进程还活着"。
    std::fprintf(stdout, "  [ok] 平台插件（QApplication 已构造）\n");

    // —— TLS 后端 ——
    // 订阅拉取、程序更新、GeoIP 下载全走 HTTPS。Windows 用 schannel、macOS 用 securetransport、
    // Linux 用 openssl（运行期 dlopen libssl.so.3）。三条路缺任意一条的表现都是
    // 「什么都下不下来」，而 Qt 只会给一句 "TLS initialization failed"。
    check(QSslSocket::supportsSsl(), "TLS 后端可用",
          QStringLiteral("backends=%1 lib=%2")
              .arg(QSslSocket::availableBackends().join(QLatin1Char(',')),
                   QSslSocket::sslLibraryVersionString()));

    // —— SQLite 驱动 ——
    // coast.db（设备台账 + 上网历史）。缺了程序照常跑，只是这两块静默失效。
    check(QSqlDatabase::isDriverAvailable(QStringLiteral("QSQLITE")), "QSQLITE 驱动",
          QStringLiteral("drivers=%1").arg(QSqlDatabase::drivers().join(QLatin1Char(','))));

    // —— 图片格式 ——
    // ico：assets/icon.ico（窗口/托盘图标）。png：Qt Quick 内部与图标合成都要。两者都在 qtbase 里。
    //
    // ★ **故意不查 svg**，这是查出来的结论不是偷懒：qrc 里那两个 chevron-*.svg 只被
    //   src/MainWindow.cpp 引用，而那是没有参与编译的 Widgets 遗留界面（见 CLAUDE.md）。
    //   QML 这边的图标全部是字体（iconfont.ttf / remixicon.ttf），一个 svg 都没用。
    //   所以静态 Qt 的模块集里**没有 qtsvg** —— 少一个模块、少一份构建时间。
    //   哪天 QML 侧真用上 .svg，这里要把 svg 加回断言，同时把 qtsvg 加回
    //   .github/actions/qt-static 的模块列表，两处缺一不可（只加前者=CI 当场红，
    //   只加后者=白编一个模块，都比"图标静默空白"好）。
    const QList<QByteArray> fmts = QImageReader::supportedImageFormats();
    QStringList fmtNames;
    for (const QByteArray &f : fmts)
        fmtNames << QString::fromLatin1(f);
    const QString fmtDetail = QStringLiteral("formats=%1").arg(fmtNames.join(QLatin1Char(',')));
    check(fmts.contains(QByteArrayLiteral("ico")), "ico 图片格式", fmtDetail);
    check(fmts.contains(QByteArrayLiteral("png")), "png 图片格式", fmtDetail);

    // —— QML 模块 ——
    // 这一条是静态构建**最关键**的守卫：共享构建下引擎是运行期从 qml/ 目录 dlopen 插件的，
    // 静态构建下没有任何东西可 open，必须由 qt_import_qml_plugins 在链接期扫描并链入。
    // 漏掉的表现是一个空窗口 —— 不报错、不崩、CI 里看不出来。
    // 这里用一个不建窗口的最小组件把三条 import 一次性验掉（QtQuick / Controls 的样式 / 本 app 自己的模块）。
    {
        QQmlEngine engine;
        QQmlComponent comp(&engine);
        comp.setData("import QtQuick\n"
                     "import QtQuick.Controls\n"
                     "import QtQuick.Layouts\n"
                     "import ClashAuto\n"
                     "QtObject { property string s: Theme.uiFont }\n",
                     QUrl(QStringLiteral("qrc:/coast/staticdeps.qml")));
        QString err;
        if (comp.isError()) {
            const auto errs = comp.errors();
            QStringList lines;
            for (const auto &e : errs)
                lines << e.toString();
            err = lines.join(QStringLiteral(" | "));
        }
        check(!comp.isError(), "QML 模块 QtQuick/Controls/Layouts/ClashAuto", err);
        if (!comp.isError()) {
            QScopedPointer<QObject> obj(comp.create());
            check(!obj.isNull(), "QML 组件可实例化（Theme 单例可访问）");
        }
    }

    if (g_fail == 0) {
        std::fprintf(stdout, "STATICDEPS-SELFTEST: 全部通过\n");
        return 0;
    }
    std::fprintf(stdout, "STATICDEPS-SELFTEST: %d 项失败\n", g_fail);
    return 1;
}
