import QtQuick
import QtQuick.Controls // ToolTip：徽章文字被 elide 时把完整文案挂在悬停提示上
import QtQuick.Layouts
import ClashAuto

// 设备列表行：类型头像 + 名称/副标题（IP·厂商）/最后访问的地址 + 实时速率 + 最右一列（代理开关，
// 或者「本机/网关/其它网络」这类不可代理的原因徽章——两者互斥，共用同一个 38px 槽位）。
// 被代理的设备行整行背景是一张实时上/下行流量图（DeviceTrafficBg）。
// 选中态整行高亮；离线行整体淡化。
// 详细信息（策略、历史用量、连接、常用域名…）不在行里，点行打开 DeviceDetailWindow。
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
    property string lastHost: ""   // 最后访问的地址（域名，没嗅探到就是目标 IP）
    property var rateUpHist: []    // 背景流量图的数据（近 N 拍速率，模型侧采样）
    property var rateDownHist: []
    property bool isSelf: false
    property bool isGateway: false
    property bool proxyable: false // 能否开代理（非本机/网关 + 在主网卡网段内）
    property bool selected: false
    signal clicked()
    signal toggleProxy()

    // 行窄到放不下速率两列时（窗口拖到很小），把它收起来只留头像/名字/开关——右侧那几列是
    // 定宽的，不收就只能溢出到行外面去。名字列 fillWidth，剩多少用多少。
    readonly property bool compact: width < 250

    // 60 而不是原来的 52：多了「最后访问」一行。**所有行等高**（有没有那行都一样），
    // 高度随内容变的话，列表每来一条新连接就会自己长高一格，滚动位置跟着跳。
    height: 60
    radius: 5
    color: selected ? Qt.rgba(72 / 255, 151 / 255, 248 / 255, 0.22)
                    : (rowMouse.containsMouse ? Theme.hover : Theme.nodeRowBg)
    opacity: online ? 1.0 : 0.5

    // 背景实时流量图（压在所有内容之下）。只有被代理的设备才有：其余设备的流量不经核心，
    // 画出来永远是贴底的 0 线。速率是 0 也照画——那正是「已接管、此刻闲着」的样子。
    DeviceTrafficBg {
        anchors.fill: parent
        visible: root.proxied
        corner: root.radius
        up: root.proxied ? root.rateUpHist : []
        down: root.proxied ? root.rateDownHist : []
    }

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

        // 名称 + 副标题 + 最后访问：**吃掉右侧信息区剩下的全部宽度**，放不下就 elide。
        // minimumWidth 显式给 0：RowLayout 空间不够时是按各项可压缩余量一起压的，右侧那几列都
        // 写了 minimumWidth = 自身宽度（刚性），这里给 0 就保证「该被挤的是名字」，而不是把右侧
        // 挤窄——那会让不同行的右侧内容落在不同的 x 上。
        // 垂直居中（而不是让 Layout 把三行拉开铺满行高）：没有「最后访问」那行的设备，
        // 剩下两行也该在行的正中间。
        ColumnLayout {
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            Layout.alignment: Qt.AlignVCenter
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
            // 最后访问的地址：设备最近新建的那条连接的目标。没有就整行收起（不占位）——
            // 大多数设备没开代理，流量不经核心，这里永远没有值。
            Text {
                Layout.fillWidth: true
                visible: root.lastHost !== ""
                text: "→ " + root.lastHost
                elide: Text.ElideRight
                font.pixelSize: 10
                color: Theme.textSecondary
            }
        }

        // ———————————————— 右侧信息区：实时速率 + 代理开关/徽章 ————————————————
        // **紧贴行的右边缘**，宽度只取自身内容：名称列 fillWidth 会把剩下的宽度全吃掉，所以这块
        // 自然被顶到最右。用不上的东西直接 visible:false 收起来（不占位），把宽度让给名字。
        //
        // 必须守住的规矩：每个子项都写死宽度并给 minimumWidth —— RowLayout 空间不够时是按各项
        // 可压缩余量**一起**压的，不给最小宽的话，副标题长的行会把右侧几列压窄一截，同一列在
        // 不同行落在不同的 x 上（实测「网关」行比「本机」行早 39px）。给了之后被挤的只会是名字。
        //
        // 「今日/累计用量」两列已经从行里拿掉了——那是详情窗（DeviceDetailWindow）的内容；
        // 行里只留「此刻在跑多快」，配合背景那张流量图。
        RowLayout {
            Layout.alignment: Qt.AlignVCenter
            spacing: 8

            // 实时速率：**常驻显示，0 也显示**（"↓ 0 B/s"）。宽度写死：速率文字每一拍都在变宽
            // 变窄（"↓ 9.77 KB/s" ↔ "↓ 1.20 MB/s"），列宽跟着变整行就在抖。
            // 闲着的时候只是淡下去（**位置和占位都不变**），不再像以前那样整列收起来。
            ColumnLayout {
                visible: !root.compact
                opacity: root.rateDown > 0 || root.rateUp > 0 ? 1.0 : 0.45
                Behavior on opacity { NumberAnimation { duration: 120 } }
                Layout.alignment: Qt.AlignVCenter
                Layout.preferredWidth: 76
                Layout.minimumWidth: 76
                spacing: 0
                Text {
                    Layout.fillWidth: true
                    text: "↓ " + Theme.fmtRate(root.rateDown)
                    font.pixelSize: 10
                    color: "#5bb44b"
                    elide: Text.ElideRight
                    horizontalAlignment: Text.AlignRight
                }
                Text {
                    Layout.fillWidth: true
                    text: "↑ " + Theme.fmtRate(root.rateUp)
                    font.pixelSize: 10
                    color: "#b14a4a"
                    elide: Text.ElideRight
                    horizontalAlignment: Text.AlignRight
                }
            }

            // 最右一列：**开关和徽章共用同一个 38px 槽位**（一台设备要么能开代理、要么有一个
            // 「为什么不能开」的理由，两者互斥）。槽位宽度写死 = 开关宽度，所以整列的左右边缘
            // 在每一行都一样齐。
            Item {
                Layout.alignment: Qt.AlignVCenter
                Layout.preferredWidth: 38
                Layout.minimumWidth: 38
                Layout.preferredHeight: 20

                // 代理开关。
                Rectangle {
                    id: proxySwitch
                    readonly property bool canToggle: root.proxied || (root.proxyable && root.online)
                    anchors.fill: parent
                    visible: root.proxyable || root.proxied
                    opacity: canToggle ? 1.0 : 0.4
                    radius: 10
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
                        hoverEnabled: true
                        cursorShape: proxySwitch.canToggle ? Qt.PointingHandCursor : Qt.ForbiddenCursor
                        onClicked: if (proxySwitch.canToggle) root.toggleProxy()
                    }
                }

                // 本机 / 网关 / 其它网络（跨网段，劫持不到）——不可代理的原因徽章，占开关的位置。
                // 「其它网络」只在设备**在线**时才敢下这个结论：inLanSubnet 不持久化（换了网络不能
                // 沿用上次的判断），所以从台账加载出来、还没被本轮扫描确认的设备一律是 false ——
                // 不加这个条件的话，刚进页面那一两秒每台离线设备都会被扣上「其它网络」的帽子。
                Rectangle {
                    id: protBadge
                    readonly property string label: root.isSelf ? qsTr("本机")
                                                  : root.isGateway ? qsTr("网关") : qsTr("其它网络")
                    visible: root.isSelf || root.isGateway || (root.online && !root.proxyable)
                    // **和开关一样宽**（就是这个槽位的宽度），不按文字伸缩：右侧一列在每行都一样。
                    // 12 种语言里总有塞不下的（德语 "Anderes Netzwerk"），塞不下就 elide，
                    // 完整文案挂在悬停提示上，详情页也另有一整句解释。
                    anchors.fill: parent
                    height: protTxt.implicitHeight + 3
                    radius: 3
                    color: Qt.rgba(0, 0, 0, 0.25)
                    Text {
                        id: protTxt
                        anchors.centerIn: parent
                        width: parent.width - 4
                        horizontalAlignment: Text.AlignHCenter
                        // 槽位是死的 38px，文案却有 12 种语言：**先缩字号再说**（9→7px），
                        // 比一上来就 elide 好——"Gateway" 只差一两个像素，截成 "Gate…" 太难看。
                        // 7px 仍塞不下的才 elide，完整文案挂在下面的悬停提示上。
                        fontSizeMode: Text.HorizontalFit
                        minimumPixelSize: 7
                        font.pixelSize: 9
                        elide: Text.ElideRight
                        text: protBadge.label
                        color: Theme.textSecondary
                    }
                    HoverHandler { id: protHover }
                    ToolTip.visible: protHover.hovered && protTxt.truncated
                    ToolTip.text: protBadge.label
                    ToolTip.delay: 300
                }
            }
        }
    }
}
