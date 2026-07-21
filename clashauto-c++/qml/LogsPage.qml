import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ClashAuto

// 日志页：两个标签「主日志 / Clash 内核」，各一个滚动时间线（LogTimeline）。
// 数据来自 context property `logModel`（LogModel）：`logModel.mainModel` = 全部日志，
// `logModel.coreModel` = 仅核心日志。样式全走 Theme，深/浅两套均适配。
Item {
    id: page

    // 复用的标签按钮（Theme 上色 + 选中态卡底 + 底部强调条）。
    component LogTab: TabButton {
        id: tabBtn
        width: implicitWidth
        padding: 0
        leftPadding: 16
        rightPadding: 16

        contentItem: Text {
            text: tabBtn.text
            font.pixelSize: 13
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            color: tabBtn.checked ? Theme.accentStrong : Theme.textSecondary
        }
        background: Rectangle {
            implicitHeight: 30
            radius: Theme.radius
            color: tabBtn.checked ? Theme.card
                                  : (tabHover.hovered ? Theme.hover : "transparent")
            HoverHandler { id: tabHover }
            // 选中标签底部强调条
            Rectangle {
                anchors.bottom: parent.bottom
                anchors.horizontalCenter: parent.horizontalCenter
                width: parent.width - 20
                height: 2
                radius: 1
                color: Theme.accent
                visible: tabBtn.checked
            }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 10
        spacing: 8

        // —————————————————— 标签栏 ——————————————————
        TabBar {
            id: bar
            Layout.fillWidth: true
            spacing: 6

            background: Rectangle { color: "transparent" }

            LogTab { text: qsTr("主日志") }
            LogTab { text: qsTr("Clash 内核") }
        }

        // —————————————————— 时间线 ——————————————————
        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: bar.currentIndex

            LogTimeline { model: logModel.mainModel }
            LogTimeline { model: logModel.coreModel }
        }
    }
}
