import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Basic
import QtQuick.Layouts
import ClashAuto

// 添加/编辑规则 —— 独立顶层窗口（任务栏可切换）。后端经 context 属性 `settings`
// （saveRule / proxyGroupNames / processChoices）。两处使用同一实例语义：
//   · 设置页「规则」tab：openWith(index, obj) 新增/编辑
//   · 连接窗右键「添加规则」：openForValue(host) 新增并把地址预填进 value
// 说明标签换行自适应宽度；类型/节点为下拉可选；类型选进程时 value 变为「可搜索的系统进程下拉」。
ApplicationWindow {
    id: win
    transientParent: null
    flags: Qt.Window
    width: 460
    height: 440
    minimumWidth: 380
    minimumHeight: 380
    title: win.editIndex >= 0 ? qsTr("编辑规则") : qsTr("新增规则")
    color: Theme.card

    property int editIndex: -1          // -1 = 新增
    // 进程候选：累积集合——每秒热更新合并当前系统进程，已结束的进程不移除（打开窗口时清空重新累积）。
    property var processNames: []
    property var processPaths: []
    readonly property var ruleTypes: ["DOMAIN-SUFFIX", "DOMAIN", "DOMAIN-KEYWORD", "DOMAIN-REGEX",
                                      "IP-CIDR", "IP-CIDR6", "GEOIP", "GEOSITE", "IP-ASN",
                                      "SRC-IP-CIDR", "SRC-PORT", "DST-PORT",
                                      "PROCESS-NAME", "PROCESS-PATH", "RULE-SET", "MATCH"]
    readonly property bool processType: {
        var t = String(typeCombo.currentText).toUpperCase()
        return t === "PROCESS-NAME" || t === "PROCESS-PATH"
    }

    function openWith(index, obj) {
        win.editIndex = index
        win.processNames = []
        win.processPaths = []
        // 类型：固定候选、只可选择；现值不在列表时前插，保证编辑旧规则也能显示。
        var t = (obj && obj.type) ? String(obj.type).toUpperCase() : "DOMAIN-SUFFIX"
        var types = win.ruleTypes.slice()
        if (types.indexOf(t) === -1)
            types.unshift(t)
        typeCombo.model = types
        typeCombo.currentIndex = types.indexOf(t)
        // 节点：候选取全部可选策略（DIRECT/REJECT + 全部策略组 + 全部订阅节点）、只可选择；现值不在列表时前插。
        var n = (obj && obj.node) ? obj.node : ""
        var nodes = settings.proxyGroupNames()
        if (n.length > 0 && nodes.indexOf(n) === -1)
            nodes.unshift(n)
        nodeCombo.model = nodes
        nodeCombo.currentIndex = n.length > 0 ? nodes.indexOf(n) : 0
        valueInput.text = (obj && obj.value) ? obj.value : ""
        win.refreshValueChoices(typeCombo.currentText)
        win.show()
        win.raise()
        win.requestActivate()
    }
    // 连接窗右键「添加规则」：新增模式，把连接地址预填进 value。
    function openForValue(value) {
        win.openWith(-1, { "type": "DOMAIN-SUFFIX", "value": value, "node": "" })
    }

    // 类型是进程时，value 变成系统进程可搜索下拉；否则纯文本输入。
    function refreshValueChoices(type) {
        var t = String(type).toUpperCase()
        if (t === "PROCESS-NAME" || t === "PROCESS-PATH")
            win.refreshProcesses()
        else
            valueInput.choices = []
    }

    // 取当前系统全部进程，合并进累积列表——只增不删（结束的进程保留可选）。
    // Windows 下 processChoices 是原生 Toolhelp32 快照（毫秒级），每秒轮询无压力。
    function refreshProcesses() {
        var isPath = String(typeCombo.currentText).toUpperCase() === "PROCESS-PATH"
        var fresh = settings.processChoices(isPath)
        var cur = (isPath ? win.processPaths : win.processNames).slice()
        var seen = {}
        for (var i = 0; i < cur.length; ++i)
            seen[cur[i]] = true
        for (var j = 0; j < fresh.length; ++j) {
            if (!seen[fresh[j]]) {
                seen[fresh[j]] = true
                cur.push(fresh[j])
            }
        }
        if (isPath)
            win.processPaths = cur
        else
            win.processNames = cur
        valueInput.choices = cur // 重新赋值（新数组引用）触发下拉过滤刷新
    }

    // 进程候选每秒热更新：仅窗口可见且类型为进程时轮询。
    Timer {
        interval: 1000
        repeat: true
        running: win.visible && win.processType
        onTriggered: win.refreshProcesses()
    }

    // ————————————————— 主题化下拉（类型/节点：只可选择，不可输入）—————————————————
    // 不用 editable+TextField：自定义 TextField 会吃掉整控件点击导致下拉打不开；
    // 非编辑 ComboBox 整个控件点击即开合弹层，且值只能从列表选。
    component ThemedCombo: ComboBox {
        id: ec
        implicitHeight: 32
        font.pixelSize: 13
        background: Rectangle {
            radius: 3
            color: Theme.inputBg
            border.width: 1
            border.color: ec.activeFocus ? Theme.accent : Theme.inputBorder
        }
        contentItem: Text {
            leftPadding: 8
            rightPadding: ec.indicator.width + 4
            text: ec.displayText
            font: ec.font
            color: Theme.textPrimary
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }
        indicator: Text {
            x: ec.width - width - 8
            y: (ec.height - height) / 2
            text: "▾"
            color: Theme.textMuted
            font.pixelSize: 12
        }
        delegate: ItemDelegate {
            width: ec.width
            height: 28
            contentItem: Text {
                text: modelData
                color: Theme.textPrimary
                font.pixelSize: 13
                verticalAlignment: Text.AlignVCenter
                leftPadding: 6
                elide: Text.ElideRight
            }
            background: Rectangle { color: highlighted ? Theme.hover : "transparent" }
            highlighted: ec.highlightedIndex === index
        }
        popup: Popup {
            y: ec.height
            width: ec.width
            implicitHeight: Math.min(contentItem.implicitHeight + 2, 240)
            padding: 1
            background: Rectangle {
                radius: 3
                color: Theme.card
                border.width: 1
                border.color: Theme.inputBorder
            }
            contentItem: ListView {
                clip: true
                implicitHeight: contentHeight
                model: ec.delegateModel
                currentIndex: ec.highlightedIndex
                ScrollBar.vertical: ScrollBar {}
            }
        }
    }

    // ————————————————— 可搜索的值输入（进程类型=系统进程下拉；否则纯文本）—————————————————
    component ValueInput: Rectangle {
        id: vin
        property alias text: field.text
        property var choices: []            // 非空=可下拉搜索；空=纯文本
        property string placeholder: ""
        implicitHeight: 32
        radius: 3
        color: Theme.inputBg
        border.width: 1
        border.color: field.activeFocus ? Theme.accent : Theme.inputBorder

        // 按输入过滤候选（空输入=全部）
        readonly property var filtered: {
            if (vin.choices.length === 0)
                return []
            var q = field.text.toLowerCase()
            if (q.length === 0)
                return vin.choices
            var out = []
            for (var i = 0; i < vin.choices.length; ++i) {
                if (String(vin.choices[i]).toLowerCase().indexOf(q) !== -1)
                    out.push(vin.choices[i])
            }
            return out
        }

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 8
            anchors.rightMargin: 4
            spacing: 2
            TextField {
                id: field
                Layout.fillWidth: true
                background: null
                color: Theme.textPrimary
                placeholderTextColor: Theme.textMuted
                placeholderText: vin.placeholder
                font.pixelSize: 13
                selectByMouse: true
                verticalAlignment: TextInput.AlignVCenter
                onActiveFocusChanged: if (activeFocus && vin.choices.length > 0) pop.open()
                onTextChanged: if (activeFocus && vin.choices.length > 0 && !pop.opened) pop.open()
                Keys.onDownPressed: if (vin.choices.length > 0) pop.open()
            }
            Text {
                visible: vin.choices.length > 0 // 仅进程类型显示下拉指示
                text: "▾"
                color: Theme.textMuted
                font.pixelSize: 12
                Layout.rightMargin: 4
                TapHandler { onTapped: pop.opened ? pop.close() : pop.open() }
            }
        }

        Popup {
            id: pop
            y: vin.height + 2
            width: vin.width
            implicitHeight: Math.min(220, list.contentHeight + 8)
            padding: 4
            background: Rectangle {
                radius: 4
                color: Theme.card
                border.width: 1
                border.color: Theme.inputBorder
            }
            contentItem: ListView {
                id: list
                clip: true
                model: vin.filtered
                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                delegate: Rectangle {
                    required property var modelData
                    required property int index
                    width: ListView.view.width
                    height: 26
                    radius: 3
                    color: ihov.hovered ? Theme.hover : "transparent"
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.left: parent.left
                        anchors.leftMargin: 6
                        anchors.right: parent.right
                        anchors.rightMargin: 6
                        text: modelData
                        elide: Text.ElideMiddle
                        font.pixelSize: 12
                        color: Theme.textSecondary
                    }
                    HoverHandler { id: ihov }
                    TapHandler {
                        onTapped: {
                            field.text = modelData
                            pop.close()
                        }
                    }
                }
            }
        }
    }

    // ————————————————— 主题化按钮 —————————————————
    component PillButton: Button {
        id: btn
        property bool primary: false
        implicitHeight: 30
        padding: 10
        font.pixelSize: 13
        contentItem: Text {
            text: btn.text
            color: btn.primary ? "white" : Theme.textPrimary
            font: btn.font
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            opacity: btn.enabled ? 1 : 0.5
        }
        background: Rectangle {
            radius: 4
            color: btn.primary ? (btn.down ? Theme.accentStrong : Theme.accent)
                               : (btn.down ? Theme.hover : Theme.inputBg)
            border.width: btn.primary ? 0 : 1
            border.color: Theme.inputBorder
        }
    }

    // 说明标签：换行 + 自适应宽度
    component FieldLabel: Label {
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        color: Theme.textSecondary
        font.pixelSize: 12
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 8

        Label {
            text: win.editIndex >= 0 ? qsTr("编辑规则") : qsTr("新增规则")
            color: Theme.textPrimary
            font.pixelSize: 16
            font.bold: true
        }

        FieldLabel { text: qsTr("类型") }
        ThemedCombo {
            id: typeCombo
            Layout.fillWidth: true
            model: win.ruleTypes
            onCurrentTextChanged: win.refreshValueChoices(currentText)
        }

        FieldLabel { text: qsTr("值（域名 / IP 段；进程规则从下拉搜索选择；MATCH 留空）") }
        ValueInput {
            id: valueInput
            Layout.fillWidth: true
            placeholder: qsTr("输入或从下拉选择")
        }

        FieldLabel { text: qsTr("节点 / 策略组") }
        ThemedCombo {
            id: nodeCombo
            Layout.fillWidth: true
        }

        Item { Layout.fillHeight: true }

        RowLayout {
            Layout.fillWidth: true
            Item { Layout.fillWidth: true }
            PillButton { text: qsTr("取消"); onClicked: win.close() }
            PillButton {
                text: qsTr("确定")
                primary: true
                onClicked: {
                    settings.saveRule(win.editIndex, typeCombo.currentText,
                                      valueInput.text, nodeCombo.currentText)
                    win.close()
                }
            }
        }
    }
}
