import QtQuick
import QtQuick.Controls
import ClashAuto

// 全 UI 统一的「下拉选择」(select)。**新增下拉一律用这个**，不要再各页面内联一份 —— 设备页
// 以前直接用裸 ComboBox（Basic 样式的白底方框），和设置页/规则编辑器的主题化下拉长得完全不一样。
//
// 不做 editable：自定义 TextField 会吃掉整控件的点击导致下拉打不开；非编辑 ComboBox 整个控件
// 点击即开合弹层，值也只能从列表里选。需要「可选可手输」的场景见 SettingsPage 的 ThemedEditCombo。
//
// 尺寸：默认 30px 高、13px 字（与设置页一致）；宽度由调用方给 Layout.preferredWidth/implicitWidth。
// 页脚那个模式下拉（Main.qml）故意不用本组件——它要跟 FooterSwitch 一样的纯黑/纯白无边框造型。
ComboBox {
    id: cb
    implicitWidth: 160
    implicitHeight: 30
    font.pixelSize: 13

    background: Rectangle {
        radius: 3
        color: Theme.inputBg
        border.width: 1
        border.color: cb.activeFocus ? Theme.accent : Theme.inputBorder
    }
    contentItem: Text {
        leftPadding: 8
        rightPadding: cb.indicator.width + 4
        text: cb.displayText
        font: cb.font
        color: Theme.textPrimary
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }
    indicator: Text {
        x: cb.width - width - 8
        y: (cb.height - height) / 2
        text: "▾"
        color: Theme.textMuted
        font.pixelSize: 12
    }
    delegate: ItemDelegate {
        required property var modelData
        required property int index
        width: cb.width
        height: 28
        highlighted: cb.highlightedIndex === index
        contentItem: Text {
            text: modelData
            color: Theme.textPrimary
            font.pixelSize: 13
            verticalAlignment: Text.AlignVCenter
            leftPadding: 6
            elide: Text.ElideRight
        }
        background: Rectangle { color: highlighted ? Theme.hover : "transparent" }
    }
    popup: Popup {
        y: cb.height
        width: cb.width
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
            model: cb.delegateModel
            currentIndex: cb.highlightedIndex
            ScrollBar.vertical: ScrollBar {}
        }
    }
}
