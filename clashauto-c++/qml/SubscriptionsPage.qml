import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ClashAuto

// 订阅页：可滚动的订阅卡列表（名称/类型/URL/节点数/启用态/更新周期）+ 动作
// （添加/编辑/删除/启用停用/更新单个/更新全部/查看节点）。行为对齐 Widgets 版
// MainWindow::buildSubscriptionsPage / createSubscriptionCard / showSubscriptionNodes。
Item {
    id: page

    // ————————————————— 图标：Remix Icon 字体字形（文本渲染，清晰可换色，全 App 通用）—————————————————
    // kind → 码点（见 assets/remixicon.css）：启用 check / 查看 eye / 编辑 edit / 更新 refresh / 删除 trash / 添加 add。
    readonly property var ic: ({
        "check":   "",
        "eye":     "",
        "pencil":  "",
        "refresh": "",
        "close":   "",
        "trash":   "",
        "plus":    ""
    })
    component LineIcon: Text {
        property string kind: ""
        property int size: 16
        font.family: Theme.riFont
        font.pixelSize: size
        color: Theme.textSecondary
        text: page.ic[kind] !== undefined ? page.ic[kind] : ""
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
    }
    component CircleBtn: Rectangle {
        id: cb
        property string kind: ""
        property string tip: ""
        property bool on: false
        property bool danger: false
        signal act()

        implicitWidth: 28
        implicitHeight: 28
        radius: 14
        color: danger
               ? (hover.hovered ? Qt.rgba(1, 0.30, 0.31, 0.28) : Qt.rgba(1, 0.30, 0.31, 0.12))
               : on
                 ? Theme.accent
                 : (hover.hovered ? Theme.hover : Theme.metricBg)
        Behavior on color { ColorAnimation { duration: 120 } }
        LineIcon {
            anchors.centerIn: parent
            size: 16
            kind: cb.kind
            color: cb.on ? "white" : (cb.danger ? Theme.danger : Theme.textSecondary)
        }
        HoverHandler { id: hover }
        TapHandler { onTapped: cb.act() }
        ToolTip.visible: hover.hovered && cb.tip.length > 0
        ToolTip.text: cb.tip
    }

    // ————————————————— 主题化文本框（对齐 StatusPage 搜索框）—————————————————
    component ThemedField: TextField {
        color: Theme.textPrimary
        placeholderTextColor: Theme.textMuted
        font.pixelSize: 13
        selectByMouse: true
        background: Rectangle {
            radius: 3
            color: Theme.inputBg
            border.width: 1
            border.color: parent.activeFocus ? Theme.accent : Theme.inputBorder
        }
    }

    // ————————————————— 主题化文字按钮（可带前置线性图标）—————————————————
    component TextBtn: Rectangle {
        id: tb
        property string label: ""
        property string kind: "" // 可选前置图标名，空则纯文字
        property bool primary: true
        signal act()
        implicitWidth: Math.max(84, row.implicitWidth + 24)
        implicitHeight: 30
        radius: Theme.radius
        color: primary
               ? (h.hovered ? Theme.accentStrong : Theme.accent)
               : (h.hovered ? Theme.hover : Theme.metricBg)
        Row {
            id: row
            anchors.centerIn: parent
            spacing: 6
            LineIcon {
                visible: tb.kind.length > 0
                size: 15
                anchors.verticalCenter: parent.verticalCenter
                kind: tb.kind
                color: tb.primary ? "white" : Theme.textSecondary
            }
            Text {
                id: txt
                anchors.verticalCenter: parent.verticalCenter
                text: tb.label
                font.pixelSize: 13
                color: tb.primary ? "white" : Theme.textSecondary
            }
        }
        HoverHandler { id: h }
        TapHandler { onTapped: tb.act() }
    }

    // 添加/编辑弹窗状态
    property int editingIndex: -1 // -1 = 添加模式

    function openAdd() {
        editingIndex = -1;
        fName.text = "";
        fUrl.text = "";
        editDialog.typeValue = "sub";
        fPeriod.text = "0";
        editDialog.open();
    }
    function openEdit(index, name, url, type, updateTime) {
        editingIndex = index;
        fName.text = name;
        fUrl.text = url;
        editDialog.typeValue = (type && type.toLowerCase() === "clash") ? "clash" : "sub";
        fPeriod.text = "" + updateTime;
        editDialog.open();
    }

    // 节点弹窗状态
    property int nodesIndex: -1
    property var nodesData: []
    function openNodes(index) {
        nodesIndex = index;
        nodesData = subs.nodesOf(index);
        nodesDialog.open();
    }
    function refreshNodes() {
        if (nodesIndex >= 0)
            nodesData = subs.nodesOf(nodesIndex);
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 0 // 页面内距 0：顶栏/列表各自设内距
        spacing: 8

        // —————————— 顶部工具栏（上左右内距 10）——————————
        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: 10
            Layout.leftMargin: 10
            Layout.rightMargin: 10
            spacing: 6

            Text {
                text: qsTr("订阅")
                font.pixelSize: 18
                color: Theme.textPrimary
            }
            Text {
                text: "(" + subs.count + ")"
                font.pixelSize: 10
                color: Theme.textMuted
                Layout.alignment: Qt.AlignBottom
                bottomPadding: 3
            }
            Item { Layout.fillWidth: true }

            TextBtn {
                label: qsTr("添加订阅")
                kind: "plus"
                primary: false
                onAct: page.openAdd()
            }
            TextBtn {
                label: qsTr("应用")
                kind: "check"
                primary: false
                onAct: subs.apply()
            }
            TextBtn {
                label: qsTr("更新全部")
                kind: "refresh"
                onAct: subs.updateAll()
            }
        }

        // —————————— 订阅卡列表：列表全宽（滚动条贴页面右缘），项自身左右内缩 10，末项距底 10 ——————————
        ListView {
            id: cardList
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: subs
            spacing: 10
            bottomMargin: 10 // 滚到底时最后一项距底部 10
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

            // 空态：无订阅时提示
            Text {
                anchors.centerIn: parent
                visible: subs.count === 0
                text: qsTr("暂无订阅，点击右上角「添加订阅」")
                font.pixelSize: 13
                color: Theme.textMuted
            }

            delegate: Card {
                x: 10 // 项左右各距列表边 10（列表本身全宽）
                width: ListView.view.width - 20
                implicitHeight: 108
                // 统一主题自适应底色（不再因启用而变蓝；启用态由左下「✓」按钮转蓝体现）
                color: Theme.metricBg

                ColumnLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 12
                    anchors.rightMargin: 10
                    anchors.topMargin: 8
                    anchors.bottomMargin: 8
                    spacing: 3

                    // 标题：[type] name
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 6
                        Rectangle {
                            radius: 4
                            color: Theme.metricBg
                            implicitWidth: typeTxt.implicitWidth + 12
                            implicitHeight: typeTxt.implicitHeight + 4
                            Layout.alignment: Qt.AlignVCenter
                            Text {
                                id: typeTxt
                                anchors.centerIn: parent
                                text: model.type
                                font.pixelSize: 11
                                color: Theme.textMuted
                            }
                        }
                        Text {
                            Layout.fillWidth: true
                            text: model.name
                            elide: Text.ElideRight
                            font.pixelSize: 14
                            font.bold: true
                            color: Theme.textPrimary
                        }
                    }

                    // URL
                    Text {
                        Layout.fillWidth: true
                        text: model.url
                        elide: Text.ElideMiddle
                        font.pixelSize: 11
                        color: Theme.textMuted
                    }

                    // 元信息：启用/总 节点 · 每 N 分钟
                    Text {
                        Layout.fillWidth: true
                        text: model.enabledNodeCount + " / " + model.nodeCount + qsTr(" 节点")
                              + (model.updateTime > 0 ? qsTr(" · 每 ") + model.updateTime + qsTr(" 分钟") : "")
                        font.pixelSize: 11
                        color: Theme.textSecondary
                    }

                    Item { Layout.fillHeight: true }

                    // 动作按钮行
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 4

                        CircleBtn {
                            kind: "check"
                            tip: qsTr("启用/停用")
                            on: model.use
                            onAct: subs.setEnabled(index, !model.use)
                        }
                        CircleBtn {
                            kind: "eye"
                            tip: qsTr("查看节点")
                            onAct: page.openNodes(index)
                        }
                        CircleBtn {
                            kind: "pencil"
                            tip: qsTr("编辑")
                            onAct: page.openEdit(index, model.name, model.url, model.type, model.updateTime)
                        }
                        CircleBtn {
                            kind: "refresh"
                            tip: qsTr("更新")
                            onAct: subs.update(index)
                        }
                        Item { Layout.fillWidth: true }
                        CircleBtn {
                            kind: "trash"
                            tip: qsTr("删除")
                            danger: true
                            onAct: {
                                page.pendingDelete = index;
                                page.pendingDeleteName = model.name;
                                confirmDialog.open();
                            }
                        }
                    }
                }
            }
        }
    }

    // ————————————————— 添加/编辑弹窗 —————————————————
    Popup {
        id: editDialog
        anchors.centerIn: Overlay.overlay
        width: 420
        modal: true
        focus: true
        padding: 0
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

        property string typeValue: "sub"

        background: Card {
            border.width: 1
            border.color: Theme.divider
        }

        contentItem: ColumnLayout {
            spacing: 10
            anchors.margins: 0

            Text {
                Layout.fillWidth: true
                Layout.topMargin: 14
                Layout.leftMargin: 16
                Layout.rightMargin: 16
                text: page.editingIndex < 0 ? qsTr("添加订阅") : qsTr("编辑订阅")
                font.pixelSize: 16
                font.bold: true
                color: Theme.textPrimary
            }

            ColumnLayout {
                Layout.leftMargin: 16
                Layout.rightMargin: 16
                Layout.fillWidth: true
                spacing: 8

                Text { text: qsTr("URL 或本地路径"); font.pixelSize: 12; color: Theme.textSecondary }
                ThemedField {
                    id: fUrl
                    Layout.fillWidth: true
                    placeholderText: "https://..."
                }
                Text { text: qsTr("名称"); font.pixelSize: 12; color: Theme.textSecondary }
                ThemedField {
                    id: fName
                    Layout.fillWidth: true
                    placeholderText: qsTr("留空则用 URL")
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 14
                    Text { text: qsTr("类型"); font.pixelSize: 12; color: Theme.textSecondary }
                    Repeater {
                        model: ["sub", "clash"]
                        delegate: Rectangle {
                            required property string modelData
                            radius: Theme.radius
                            implicitWidth: 60
                            implicitHeight: 26
                            color: editDialog.typeValue === modelData ? Theme.accent : Theme.metricBg
                            Text {
                                anchors.centerIn: parent
                                text: parent.modelData
                                font.pixelSize: 12
                                color: editDialog.typeValue === parent.modelData ? "white" : Theme.textSecondary
                            }
                            TapHandler { onTapped: editDialog.typeValue = parent.modelData }
                        }
                    }
                    Item { Layout.fillWidth: true }
                }

                RowLayout {
                    Layout.fillWidth: true
                    visible: page.editingIndex >= 0 // 仅编辑时可设自动更新周期
                    spacing: 10
                    Text {
                        text: qsTr("自动更新(分钟, 0=全局)")
                        font.pixelSize: 12
                        color: Theme.textSecondary
                    }
                    ThemedField {
                        id: fPeriod
                        Layout.preferredWidth: 90
                        text: "0"
                        inputMethodHints: Qt.ImhDigitsOnly
                        validator: IntValidator { bottom: 0; top: 1440 }
                    }
                    Item { Layout.fillWidth: true }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16
                Layout.bottomMargin: 14
                Layout.topMargin: 4
                spacing: 8
                Item { Layout.fillWidth: true }
                TextBtn {
                    label: qsTr("取消")
                    primary: false
                    onAct: editDialog.close()
                }
                TextBtn {
                    label: qsTr("保存")
                    onAct: {
                        if (fUrl.text.trim().length === 0)
                            return;
                        if (page.editingIndex < 0)
                            subs.add(fName.text, fUrl.text, editDialog.typeValue);
                        else
                            subs.edit(page.editingIndex, fName.text, fUrl.text,
                                      editDialog.typeValue, parseInt(fPeriod.text || "0"));
                        editDialog.close();
                    }
                }
            }
        }
    }

    // ————————————————— 删除确认弹窗 —————————————————
    property int pendingDelete: -1
    property string pendingDeleteName: ""
    Popup {
        id: confirmDialog
        anchors.centerIn: Overlay.overlay
        width: 340
        modal: true
        focus: true
        padding: 0
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        background: Card {
            border.width: 1
            border.color: Theme.divider
        }
        contentItem: ColumnLayout {
            spacing: 14
            Text {
                Layout.fillWidth: true
                Layout.topMargin: 18
                Layout.leftMargin: 18
                Layout.rightMargin: 18
                text: qsTr("确定删除订阅「") + page.pendingDeleteName + qsTr("」?")
                wrapMode: Text.Wrap
                font.pixelSize: 14
                color: Theme.textPrimary
            }
            RowLayout {
                Layout.fillWidth: true
                Layout.leftMargin: 18
                Layout.rightMargin: 18
                Layout.bottomMargin: 16
                spacing: 8
                Item { Layout.fillWidth: true }
                TextBtn {
                    label: qsTr("取消")
                    primary: false
                    onAct: confirmDialog.close()
                }
                Rectangle {
                    implicitWidth: 84
                    implicitHeight: 30
                    radius: Theme.radius
                    color: dh.hovered ? Qt.darker(Theme.danger, 1.15) : Theme.danger
                    Text {
                        anchors.centerIn: parent
                        text: qsTr("删除")
                        font.pixelSize: 13
                        color: "white"
                    }
                    HoverHandler { id: dh }
                    TapHandler {
                        onTapped: {
                            if (page.pendingDelete >= 0)
                                subs.remove(page.pendingDelete);
                            confirmDialog.close();
                        }
                    }
                }
            }
        }
    }

    // ————————————————— 节点列表弹窗 —————————————————
    Popup {
        id: nodesDialog
        anchors.centerIn: Overlay.overlay
        width: Math.min(620, page.width - 40)
        height: Math.min(460, page.height - 40)
        modal: true
        focus: true
        padding: 0
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        background: Card {
            border.width: 1
            border.color: Theme.divider
        }

        contentItem: ColumnLayout {
            spacing: 8

            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: 12
                Layout.leftMargin: 14
                Layout.rightMargin: 14
                Text {
                    text: qsTr("订阅节点")
                    font.pixelSize: 16
                    font.bold: true
                    color: Theme.textPrimary
                }
                Text {
                    text: "(" + page.nodesData.length + ")"
                    font.pixelSize: 11
                    color: Theme.textMuted
                    Layout.alignment: Qt.AlignBottom
                    bottomPadding: 2
                }
                Item { Layout.fillWidth: true }
            }

            Text {
                Layout.fillWidth: true
                Layout.leftMargin: 14
                Layout.rightMargin: 14
                visible: page.nodesData.length === 0
                text: qsTr("暂无节点，请先点击「更新」")
                font.pixelSize: 13
                color: Theme.textMuted
                horizontalAlignment: Text.AlignHCenter
                Layout.topMargin: 30
            }

            ListView {
                id: nodeList
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.leftMargin: 14
                Layout.rightMargin: 14
                clip: true
                model: page.nodesData
                spacing: 6
                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

                delegate: Rectangle {
                    required property var modelData
                    required property int index
                    width: ListView.view.width
                    height: 46
                    radius: 4
                    color: modelData.use ? Theme.nodeRowActive : Theme.nodeRowBg

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 10
                        anchors.rightMargin: 6
                        spacing: 8
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 1
                            Text {
                                Layout.fillWidth: true
                                text: modelData.name
                                elide: Text.ElideRight
                                font.pixelSize: 12
                                color: Theme.textPrimary
                            }
                            Text {
                                Layout.fillWidth: true
                                text: modelData.server + ":" + modelData.port
                                elide: Text.ElideRight
                                font.pixelSize: 10
                                color: Theme.textMuted
                            }
                        }
                        Rectangle {
                            Layout.preferredWidth: 76
                            Layout.preferredHeight: 30
                            radius: Theme.radius
                            color: th.hovered ? Theme.hover : Theme.metricBg
                            Text {
                                anchors.centerIn: parent
                                text: modelData.use ? qsTr("停用") : qsTr("使用")
                                font.pixelSize: 12
                                color: Theme.textSecondary
                            }
                            HoverHandler { id: th }
                            TapHandler {
                                onTapped: {
                                    subs.setNodeEnabled(page.nodesIndex, index, !modelData.use);
                                    page.refreshNodes();
                                }
                            }
                        }
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.leftMargin: 14
                Layout.rightMargin: 14
                Layout.bottomMargin: 12
                spacing: 8
                TextBtn {
                    label: qsTr("全选")
                    primary: false
                    onAct: {
                        subs.setAllNodesEnabled(page.nodesIndex, true);
                        page.refreshNodes();
                    }
                }
                TextBtn {
                    label: qsTr("全不选")
                    primary: false
                    onAct: {
                        subs.setAllNodesEnabled(page.nodesIndex, false);
                        page.refreshNodes();
                    }
                }
                Item { Layout.fillWidth: true }
                TextBtn {
                    label: qsTr("关闭")
                    onAct: nodesDialog.close()
                }
            }
        }
    }
}
