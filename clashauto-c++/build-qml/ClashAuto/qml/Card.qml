import QtQuick
import ClashAuto

// 不透明圆角内容卡。mac 毛玻璃下这是唯一的实底面（浮在玻璃上）。子元素直接写进来即可。
Rectangle {
    color: Theme.card
    radius: Theme.radius
}
