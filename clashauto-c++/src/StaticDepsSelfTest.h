#pragma once

// 「插件真的在这个二进制里吗」自测（COAST_STATICDEPS_SELFTEST=1，退出码 0 = 通过）。
//
// 静态 Qt 下插件必须在**链接期**被引入（qt_import_qml_plugins / Qt 的默认插件导入）。
// 漏掉不会有任何编译或链接错误，只会在运行时少掉一块功能，而且几乎全是「不崩」的那种：
//   · imageformats/svg 缺失 → 下拉箭头等 svg 图标空白，其余界面正常
//   · TLS 后端缺失         → 订阅拉不到、更新查不了，报错读起来像"网络不通"
//   · QSQLITE 缺失         → 上网历史静默不记录（本仓库已为它写过两次 CI 警告，说明确实会漏）
//   · QML 插件缺失         → loadFromModule("ClashAuto","Main") 失败，一个空窗口
//
// 共享构建下同样有用：那时它验的是 windeployqt/macdeployqt/手工拷贝有没有把插件带全 ——
// 换句话说，这条自测两种 Qt 都该跑，不要用 #ifdef 把它关掉。
//
// 需要已构造好的 QApplication（要 QML 引擎和平台插件），所以调用点在 main 里、构造之后。
int runStaticDepsSelfTest();
