import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ClashAuto

// 全部连接 —— 独立窗口，复刻旧项目 showConnectionsDialog / connections.vue：
// 标题「连接」720×480；顶部 Online(N)/Offline(N) 切换 + Search；卡片列表：
// ● 圆点(在线绿/离线灰) + [type] host + 三徽标(代理链深底白字 / 下载绿底#333 / 上传红底#333，
// 各带 iconfont 图标) + ✕ 删除(离线禁用)；速度无空格「1.50KB」。
// 圆角只用整块 radius（避免 6.7+ 才有的 per-corner radius，本地 6.5/打包 6.8 一致）。
ApplicationWindow {
    id: win
    width: 720
    height: 480
    minimumWidth: 480
    minimumHeight: 320
    title: qsTr("连接")
    color: Theme.card

    property string query: ""
    property bool showOnline: true
    property bool showOffline: true

    function spd(v) {
        var n = v < 0 ? 0 : v;
        var u = ["B", "KB", "MB", "GB", "TB"];
        var i = 0;
        while (n >= 1024.0 && i < 4) { n /= 1024.0; ++i; }
        return n.toFixed(2) + u[i];
    }

    // 过滤状态推给 C++ 模型（增量套用）：开关/搜索变化 + 窗口打开时都同步一次。
    function applyFilter() {
        bridge.connectionsModel.setFilter(win.showOnline, win.showOffline, win.query);
    }
    onShowOnlineChanged: win.applyFilter()
    onShowOfflineChanged: win.applyFilter()
    onQueryChanged: win.applyFilter()

    onVisibleChanged: {
        if (visible) {
            win.applyFilter();
            bridge.refreshConnections();
            refresh.start();
        } else {
            refresh.stop();
        }
    }
    Timer {
        id: refresh
        interval: 1000
        repeat: true
        onTriggered: bridge.refreshConnections()
    }

    component ConnBadge: Rectangle {
        id: badge
        property string glyph: ""
        property string label: ""
        property color bg: "#000000"
        property color fg: "#ffffff"
        radius: 5
        color: badge.bg
        implicitWidth: badgeRow.implicitWidth + 12
        implicitHeight: 22
        Row {
            id: badgeRow
            anchors.centerIn: parent
            spacing: 4
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: badge.glyph
                font.family: Theme.iconFont
                font.pixelSize: 12
                color: badge.fg
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: badge.label
                font.pixelSize: 12
                color: badge.fg
            }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 5
        spacing: 5

        // —— 顶部工具栏：Online/Offline 切换 + Search ——
        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 5
            Layout.rightMargin: 5
            spacing: 10

            // 分段按钮组：离线段左端塞到在线段底下 3px（重叠），中间无缝、只外侧圆角。
            Item {
                Layout.preferredWidth: onSeg.width + offSeg.width - 3
                Layout.preferredHeight: 26
                Rectangle {
                    id: offSeg
                    x: onSeg.width - 3
                    width: offLbl.implicitWidth + 27
                    height: 26
                    radius: 3
                    z: 0 // 在下：左端圆角被在线段盖住
                    color: win.showOffline ? "#4898f8" : "#909399"
                    Text {
                        id: offLbl
                        x: (parent.width - width) / 2 + 1.5
                        anchors.verticalCenter: parent.verticalCenter
                        text: "Offline (" + (bridge.connectionsModel.count - bridge.connectionsModel.onlineTotal) + ")"
                        color: "#fff"
                        font.pixelSize: 12
                    }
                    TapHandler { onTapped: win.showOffline = !win.showOffline }
                }
                Rectangle {
                    id: onSeg
                    x: 0
                    width: onLbl.implicitWidth + 24
                    height: 26
                    radius: 3
                    z: 1 // 在上：盖住离线段左端
                    color: win.showOnline ? "#4898f8" : "#909399"
                    Text {
                        id: onLbl
                        anchors.centerIn: parent
                        text: "Online (" + bridge.connectionsModel.onlineTotal + ")"
                        color: "#fff"
                        font.pixelSize: 12
                    }
                    TapHandler { onTapped: win.showOnline = !win.showOnline }
                }
            }

            // Search：整块 radius 圆角，前缀标签 + 输入框（均透明，露外框底）
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 26
                radius: 3
                color: Theme.dark ? "#444444" : "#eaeaea"
                border.width: 1
                border.color: Theme.dark ? "#333333" : "#cccccc"
                Row {
                    anchors.fill: parent
                    anchors.margins: 1
                    Item {
                        width: searchLbl.implicitWidth + 20
                        height: parent.height
                        Text {
                            id: searchLbl
                            anchors.centerIn: parent
                            text: qsTr("Search")
                            color: Theme.dark ? "#ffffff" : "#333333"
                            font.pixelSize: 12
                        }
                    }
                    Rectangle { width: 1; height: parent.height; color: Theme.dark ? "#333333" : "#cccccc" }
                    TextField {
                        width: parent.width - searchLbl.implicitWidth - 21
                        height: parent.height
                        color: Theme.dark ? "#ffffff" : "#333333"
                        font.pixelSize: 12
                        leftPadding: 8
                        verticalAlignment: Text.AlignVCenter
                        background: Item {}
                        onTextChanged: win.query = text
                    }
                }
            }
        }

        // —— 可滚动卡片列表 ——
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            ListView {
                anchors.fill: parent
                anchors.margins: 5
                clip: true
                model: bridge.connectionsModel
                spacing: 8
                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

                delegate: Rectangle {
                    width: ListView.view.width
                    height: 42
                    radius: 5
                    color: Theme.dark ? "#222222" : "#eeeeee"

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 10
                        anchors.rightMargin: 10
                        spacing: 10

                        Text {
                            text: "●"
                            font.pixelSize: 10
                            color: model.offline ? "#999999" : "#67c23a"
                        }
                        Text {
                            Layout.fillWidth: true
                            text: "[" + model.type + "] " + model.host
                            font.pixelSize: 14
                            elide: Text.ElideRight
                            color: model.offline ? "#999999" : (Theme.dark ? "#eeeeee" : "#333333")
                        }

                        ConnBadge { glyph: String.fromCharCode(0xe6bc); label: model.chain; bg: Qt.rgba(0, 0, 0, 0.35); fg: "#ffffff" }
                        ConnBadge { glyph: String.fromCharCode(0xe6cd); label: model.download > 0 ? win.spd(model.download) : "-"; bg: Qt.rgba(0, 1, 0, 0.5); fg: "#333333" }
                        ConnBadge { glyph: String.fromCharCode(0xe6cc); label: model.upload > 0 ? win.spd(model.upload) : "-"; bg: Qt.rgba(1, 0, 0, 0.5); fg: "#333333" }

                        Rectangle {
                            Layout.preferredWidth: 30
                            Layout.preferredHeight: 30
                            radius: 3
                            enabled: !model.offline
                            color: delHover.hovered && !model.offline ? "#f56c6c"
                                 : (Theme.dark ? "#333333" : "#eeeeee")
                            border.width: Theme.dark ? 0 : 1
                            border.color: "#dddddd"
                            Text {
                                anchors.centerIn: parent
                                text: "✕"
                                font.pixelSize: 12
                                color: model.offline ? (Theme.dark ? "#666666" : "#cccccc")
                                     : (delHover.hovered ? "#ffffff" : "#f56c6c")
                            }
                            HoverHandler { id: delHover }
                            TapHandler {
                                enabled: !model.offline
                                onTapped: bridge.closeConnectionById(model.connId)
                            }
                        }
                    }
                }
            }
        }
    }
}
