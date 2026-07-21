import QtQuick
import QtQuick.Layouts
import ClashAuto

// 节点列表行：名称（省略号）+ 延迟/速度药丸 + 应用按钮。活动行整行高亮（无左竖条）。
Rectangle {
    id: root
    property string display: ""
    property string badgeText: "-"
    property color badgeColor: "#33ffffff"
    property bool active: false
    property bool switching: false    // 是否有切换/禁用在途（此时所有行按钮禁用，对齐旧项目 disableLoading）
    property bool isTarget: false     // 本行是否为切换目标（是则按钮显示转圈帧）
    property string spinnerText: ""   // 当前转圈帧 ◐◓◑◒（由 bridge.spinnerGlyph 驱动）
    signal apply()   // 非活动行：应用/切换到该节点
    signal disable() // 活动行：禁用当前正在使用的节点（从池中移除并重建配置）

    height: 40
    radius: 4
    color: active ? Theme.nodeRowActive : Theme.nodeRowBg

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 8
        spacing: 8

        Text {
            Layout.fillWidth: true
            text: root.display
            elide: Text.ElideRight
            font.pixelSize: 12
            color: Theme.textSecondary
        }

        Rectangle {
            radius: 5
            color: root.badgeColor
            implicitWidth: badge.implicitWidth + 10
            implicitHeight: badge.implicitHeight + 6
            Layout.alignment: Qt.AlignVCenter
            Text {
                id: badge
                anchors.centerIn: parent
                text: root.badgeText
                font.pixelSize: 12
                color: "#222222"
            }
        }

        // 单个按钮：非活动项「应用」= 切换到该节点；活动（正在使用）项「禁用」= 禁用其当前使用的节点。
        // 对齐旧项目 createNodeRow：只有正在使用的节点才显示「禁用」，两态都可单击。
        // 切换在途：目标行显示转圈帧，其余行文字变灰、全部不可点（对齐旧项目 disableLoading + 目标转圈）。
        Rectangle {
            Layout.preferredWidth: 82
            Layout.fillHeight: true
            color: (applyHover.hovered && !root.switching) ? Theme.hover : "transparent"
            Text {
                anchors.centerIn: parent
                text: root.isTarget ? root.spinnerText
                                    : (root.active ? qsTr("禁用") : qsTr("应用"))
                font.pixelSize: 12
                color: Theme.textSecondary
                // 切换在途且非目标行：文字变灰表示不可点（目标行满亮以突出转圈）
                opacity: (root.switching && !root.isTarget) ? 0.35 : 1.0
            }
            HoverHandler { id: applyHover }
            TapHandler {
                enabled: !root.switching // 切换在途忽略所有点击（对齐旧项目 clicked 槽的防重入 return）
                onTapped: root.active ? root.disable() : root.apply()
            }
        }
    }
}
