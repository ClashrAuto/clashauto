import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ClashAuto

// 日志时间线：不透明卡内一个滚动 ListView，复刻 Widgets 版 appendTimeline 的观感——
// 左侧竖轴 + 彩色圆点（红=失败/错误、橙=警告、绿=成功/完成、蓝=信息），右侧时间戳 + 正文。
// 最新在顶部（model 第 0 行即最新），新条目到达自动滚到顶。原生悬浮滚动条。
Card {
    id: root

    // 由 LogsPage 传入：LogEntryModel（角色 text / time / severity）。
    property alias model: view.model

    // severity 字符串 → 固定品牌色（对齐 Widgets 版 appendTimeline 的取色）。
    function dotColor(sev) {
        if (sev === "error")
            return "#f56c6c";
        if (sev === "warn")
            return "#e6a23c";
        if (sev === "success")
            return "#67c23a";
        return "#4898f8"; // info
    }

    ListView {
        id: view
        anchors.fill: parent
        anchors.margins: 6
        clip: true
        spacing: 0
        rightMargin: 8 // 行右缘离卡片边 8px（滚动条悬浮其上）
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

        // 新日志插到第 0 行 → 自动把视图滚到顶部（对齐 Widgets：滚动条置最小值）。
        onCountChanged: positionViewAtBeginning()

        delegate: Item {
            width: ListView.view.width - ListView.view.rightMargin
            implicitHeight: Math.max(rail.implicitHeight, texts.implicitHeight)

            RowLayout {
                anchors.fill: parent
                spacing: 8

                // —— 竖轴 + 圆点 ——
                Item {
                    id: rail
                    Layout.preferredWidth: 14
                    Layout.fillHeight: true
                    implicitHeight: texts.implicitHeight

                    Rectangle { // 竖轴
                        x: 6
                        width: 2
                        height: parent.height
                        color: Theme.divider
                    }
                    Rectangle { // 圆点（对齐 Widgets：圆心 y≈9）
                        x: 3
                        y: 5
                        width: 8
                        height: 8
                        radius: 4
                        color: root.dotColor(model.severity)
                    }
                }

                // —— 时间戳 + 正文 ——
                ColumnLayout {
                    id: texts
                    Layout.fillWidth: true
                    Layout.topMargin: 4
                    Layout.bottomMargin: 10
                    spacing: 2

                    Text {
                        text: model.time
                        font.pixelSize: 11
                        color: Theme.textMuted
                    }
                    Text {
                        Layout.fillWidth: true
                        text: model.text
                        font.pixelSize: 13
                        color: Theme.textSecondary
                        wrapMode: Text.Wrap
                    }
                }
            }
        }

        Text {
            anchors.centerIn: parent
            visible: view.count === 0
            text: qsTr("暂无日志")
            font.pixelSize: 13
            color: Theme.textMuted
        }
    }
}
