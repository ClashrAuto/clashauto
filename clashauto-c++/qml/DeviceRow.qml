import QtQuick
import QtQuick.Layouts
import ClashAuto

// 设备列表行：类型头像 + 名称/副标题（IP·厂商）+ 实时速率 + 代理开关。
// 选中态整行高亮；离线行整体淡化。本机/网关不显示代理开关（改显「保护」徽章）。
Rectangle {
    id: root
    property string mac: ""
    property string ip: ""
    property string name: ""
    property string typeKey: "unknown"
    property string vendor: ""
    property bool online: false
    property bool proxied: false
    property real rateUp: 0
    property real rateDown: 0
    property real todayUp: 0    // 今日累计上行字节
    property real todayDown: 0  // 今日累计下行字节
    property bool isSelf: false
    property bool isGateway: false
    property bool proxyable: false // 能否开代理（非本机/网关 + 在主网卡网段内）
    property bool selected: false
    signal clicked()
    signal toggleProxy()

    height: 52
    radius: 5
    color: selected ? Qt.rgba(72 / 255, 151 / 255, 248 / 255, 0.22)
                    : (rowMouse.containsMouse ? Theme.hover : Theme.nodeRowBg)
    opacity: online ? 1.0 : 0.5

    // 整行用 MouseArea 而不是 TapHandler：TapHandler 只拿「被动 grab」，按下的一瞬间 Main.qml
    // 那个铺满窗口的背景 DragHandler 就能接管 → 按住列表往下拖会把**整个窗口**拖走（实测：拖一次
    // 窗口位移 58px、列表只滚了 2px）。MouseArea 在按下时拿独占 grab，而那个 DragHandler 的
    // grabPermissions 只有 Approves* 位、没有 CanTakeOverFrom*，抢不走 → 窗口不动；
    // ListView(Flickable) 仍可从 MouseArea 手里抢 grab（preventStealing 默认 false）→ 列表照常滚。
    MouseArea {
        id: rowMouse
        anchors.fill: parent
        hoverEnabled: true
        onClicked: root.clicked()
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 8
        anchors.rightMargin: 8
        spacing: 8

        // 类型头像（纯色圆角 + 类型图标）+ 在线小圆点
        Item {
            Layout.preferredWidth: 34
            Layout.minimumWidth: 34 // 同右侧各列：不给最小宽，长副标题的行会把头像也压扁
            Layout.preferredHeight: 34
            Layout.alignment: Qt.AlignVCenter
            Rectangle {
                anchors.fill: parent
                radius: 8
                color: Theme.deviceColor(root.typeKey)
                Text {
                    anchors.centerIn: parent
                    text: Theme.deviceGlyph(root.typeKey)
                    font.family: Theme.riFont // 图标字体（不继承默认的 MiSans，否则是空白方块）
                    font.pixelSize: 18
                    color: "#ffffff"
                }
            }
            Rectangle { // 在线状态角标
                width: 11; height: 11; radius: 6
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.rightMargin: -2
                anchors.bottomMargin: -2
                color: root.online ? "#4da13e" : "#888888"
                border.width: 2
                border.color: Theme.card
            }
        }

        // 名称 + 副标题
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 1
            Text {
                Layout.fillWidth: true
                text: root.name
                elide: Text.ElideRight
                font.pixelSize: 13
                color: Theme.textPrimary
            }
            Text {
                Layout.fillWidth: true
                text: root.ip + (root.vendor ? "  ·  " + root.vendor : "")
                elide: Text.ElideRight
                font.pixelSize: 10
                color: Theme.textMuted
            }
        }

        // ———————————————— 右侧信息区：徽章 + 今日 + 实时速率 + 代理开关 ————————————————
        // **整块固定宽度，内部全部用 anchors 从右往左排**，不参与 RowLayout 的伸缩分配。
        //
        // 之前这四样是 RowLayout 里的四个独立项，各自给了固定的 preferredWidth，结果仍然对不齐：
        // RowLayout 在空间不够时会按各项的可压缩余量**一起**压（不是只压 fillWidth 的名称列），
        // 而每行副标题长短不同 → 每行压缩量不同 → 徽章和流量数字在不同的行落在不同的 x 上。
        // 实测「网关」那行的徽章右边缘比「本机」那行早 39px。给每项补 minimumWidth 也没能治住，
        // 索性把整块从布局系统里摘出来：一个宽度写死的 Item + 内部 anchors 手工定位，
        // 每一行的右侧就必然逐像素一致。名称列吃掉剩下的全部宽度，长了就 elide。
        Item {
            Layout.preferredWidth: 64 + 8 + 82 + 8 + 76 + 8 + 38 // 徽章|今日|速率|开关 = 284
            Layout.minimumWidth: Layout.preferredWidth
            Layout.fillHeight: true

            // 代理开关（最右）——本机/网关/跨网段的行不画，但**位置照占**，否则这些行右侧少一列。
            Rectangle {
                id: proxySwitch
                readonly property bool canToggle: root.proxied || (root.proxyable && root.online)
                readonly property bool applicable: root.proxyable || root.proxied
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                width: 38; height: 20; radius: 10
                opacity: applicable ? (canToggle ? 1.0 : 0.4) : 0.0
                color: root.proxied ? Theme.accent : Theme.switchTrackOff
                Behavior on color { ColorAnimation { duration: 120 } }
                Rectangle {
                    width: 16; height: 16; radius: 8
                    y: 2
                    x: root.proxied ? parent.width - width - 2 : 2
                    color: "#ffffff"
                    Behavior on x { NumberAnimation { duration: 120 } }
                }
                // 用 MouseArea 而非 TapHandler：它压在整行 MouseArea 之上，按下即独占 grab ——
                // 既保证点开关不会连带选中整行，也保证在开关上按住拖动不会去拖窗口。
                MouseArea {
                    anchors.fill: parent
                    // 不适用时这块是看不见的占位：必须连带禁用，否则它白白吃掉那 38px 上的点击，
                    // 本机/网关那几行的右端就点不中了。
                    enabled: proxySwitch.applicable
                    hoverEnabled: true
                    cursorShape: proxySwitch.canToggle ? Qt.PointingHandCursor : Qt.ForbiddenCursor
                    onClicked: if (proxySwitch.canToggle) root.toggleProxy()
                }
            }

            // 实时速率（在线且有流量时才「亮起来」）。用 opacity 而不是 visible 显隐：速率文字
            // 每一拍都在变宽变窄（"↓ 9.77 KB/s" ↔ "↓ 1.20 MB/s"），位置必须钉死，否则一跑流量
            // 整行数字就左右抽搐。
            Column {
                id: rateCol
                anchors.right: proxySwitch.left
                anchors.rightMargin: 8
                anchors.verticalCenter: parent.verticalCenter
                width: 76
                spacing: 0
                opacity: root.online && (root.rateDown > 0 || root.rateUp > 0) ? 1.0 : 0.0
                Behavior on opacity { NumberAnimation { duration: 120 } }
                Text {
                    width: parent.width
                    text: "↓ " + Theme.fmtRate(root.rateDown)
                    font.pixelSize: 10
                    color: "#5bb44b"
                    elide: Text.ElideRight
                    horizontalAlignment: Text.AlignRight
                }
                Text {
                    width: parent.width
                    text: "↑ " + Theme.fmtRate(root.rateUp)
                    font.pixelSize: 10
                    color: "#b14a4a"
                    elide: Text.ElideRight
                    horizontalAlignment: Text.AlignRight
                }
            }

            // 今日累计上/下行（也是列表的排序主键，见 DeviceListModel::buildTarget）。
            // 用「今日」而不是「累计」：跨会话的历史总量对「谁在占带宽」没有参考价值。
            // 一整天都是 0 的设备（没开代理 → 流量不经 mihomo，压根统计不到）不显示「0 B」噪音。
            Column {
                id: todayCol
                anchors.right: rateCol.left
                anchors.rightMargin: 8
                anchors.verticalCenter: parent.verticalCenter
                width: 82
                spacing: 0
                opacity: (root.todayDown + root.todayUp) > 0 ? 1.0 : 0.0
                Text {
                    width: parent.width
                    text: "↓ " + Theme.fmtBytes(root.todayDown)
                    font.pixelSize: 10
                    color: Theme.textMuted
                    elide: Text.ElideRight
                    horizontalAlignment: Text.AlignRight
                }
                Text {
                    width: parent.width
                    text: "↑ " + Theme.fmtBytes(root.todayUp)
                    font.pixelSize: 10
                    color: Theme.textMuted
                    elide: Text.ElideRight
                    horizontalAlignment: Text.AlignRight
                }
            }

            // 本机 / 网关 / 其它网络（跨网段，劫持不到）——不可代理的原因徽章，右对齐。
            Rectangle {
                anchors.right: todayCol.left
                anchors.rightMargin: 8
                anchors.verticalCenter: parent.verticalCenter
                visible: root.isSelf || root.isGateway || !root.proxyable
                width: Math.min(64, protTxt.implicitWidth + 8)
                height: protTxt.implicitHeight + 3
                radius: 3
                color: Qt.rgba(0, 0, 0, 0.25)
                Text {
                    id: protTxt
                    anchors.centerIn: parent
                    width: Math.min(parent.width - 8, implicitWidth)
                    elide: Text.ElideRight
                    text: root.isSelf ? qsTr("本机")
                        : root.isGateway ? qsTr("网关") : qsTr("其它网络")
                    font.pixelSize: 9
                    color: Theme.textSecondary
                }
            }
        }
    }
}
