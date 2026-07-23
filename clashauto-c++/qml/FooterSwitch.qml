import QtQuick
import ClashAuto

// 页脚开关（增强/网页/核心）：左侧呼吸圆点 + 文字。on 决定圆点点亮。
// 宽度随标签自适应（原固定 66 只够 2 个汉字），但封顶到 maxLabelWidth，超长译文省略号，
// 既不溢出页脚也不把日志/模式挤没（页脚日志 fillWidth 会让出空间）。
Rectangle {
    id: root
    property string label: ""
    property bool on: false
    signal clicked()

    readonly property int maxLabelWidth: 80 // 标签最大显示宽（约 8~9 字符），超出省略号
    implicitWidth: 8 + dot.width + 6 + labelText.width + 10 // leftMargin+圆点+spacing+文字+右留白
    implicitHeight: 28
    radius: 3
    color: hover.hovered ? Qt.lighter(Theme.card, Theme.dark ? 2.2 : 0.97) : Theme.card

    Row {
        anchors.left: parent.left
        anchors.leftMargin: 8
        anchors.verticalCenter: parent.verticalCenter
        spacing: 6

        Rectangle {
            id: dot
            width: 12
            height: 12
            radius: 6
            anchors.verticalCenter: parent.verticalCenter
            color: root.on ? Theme.accent : Theme.switchTrackOff
            border.width: 3

            // 呼吸相位（0..1），仅启用时由动画驱动；停用时外环用静态灰。
            property real pulse: 0
            // 外环底色：启用时随 pulse 在「蓝 rgba(72,152,248,.5)」↔「灰 rgba(102,102,102,.15)」间脉动
            // （对齐原项目 @keyframes color_black：blue→gray→blue，1s infinite），停用时静态灰。
            border.color: root.on
                ? Qt.rgba((72 + 30 * pulse) / 255, (152 - 50 * pulse) / 255,
                          (248 - 146 * pulse) / 255, 0.5 - 0.35 * pulse)
                : Qt.rgba(0.4, 0.4, 0.4, 0.15)

            SequentialAnimation on pulse {
                running: root.on
                loops: Animation.Infinite
                NumberAnimation { from: 0; to: 1; duration: 500; easing.type: Easing.InOutSine }
                NumberAnimation { from: 1; to: 0; duration: 500; easing.type: Easing.InOutSine }
            }
        }
        Text {
            id: labelText
            anchors.verticalCenter: parent.verticalCenter
            text: root.label
            font.pixelSize: 12
            color: Theme.textPrimary
            // 短标签按本身宽、长标签封顶到 maxLabelWidth 并省略号（implicitWidth 为文字本征宽，不成环）
            width: Math.min(implicitWidth, root.maxLabelWidth)
            elide: Text.ElideRight
        }
    }

    HoverHandler { id: hover }
    TapHandler { onTapped: root.clicked() }
}
