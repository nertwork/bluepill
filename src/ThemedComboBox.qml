import QtQuick
import QtQuick.Controls

// ComboBox drawn with the Omarchy theme (the default style paints a white
// native-looking box and popup). `theme` is the app's context property.
ComboBox {
    id: control
    implicitHeight: 34
    font.pixelSize: theme.fontSize - 1

    contentItem: Text {
        leftPadding: 10
        rightPadding: control.indicator.width + 6
        text: control.displayText !== "" ? control.displayText : control.currentText
        font: control.font
        color: control.enabled ? theme.brightForeground : theme.darkForeground
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }

    indicator: Text {
        x: control.width - width - 10
        y: (control.height - height) / 2
        text: control.popup.visible ? "▴" : "▾"
        font.pixelSize: theme.fontSize + 4
        color: control.hovered || control.popup.visible ? theme.accent : theme.foreground
    }

    background: Rectangle {
        radius: 6
        color: control.hovered ? theme.selection : theme.lighterBackground
        border.width: 1
        border.color: (control.activeFocus || control.popup.visible) ? theme.accent : theme.muted
    }

    delegate: ItemDelegate {
        id: row
        required property var modelData
        required property int index
        width: ListView.view ? ListView.view.width : control.width
        highlighted: control.highlightedIndex === index
        text: control.textRole ? (row.modelData[control.textRole] ?? "") : String(row.modelData)
        contentItem: Text {
            text: row.text
            font: control.font
            color: row.highlighted || control.currentIndex === row.index ? theme.accent : theme.foreground
            elide: Text.ElideRight
            verticalAlignment: Text.AlignVCenter
        }
        background: Rectangle {
            color: row.highlighted ? theme.selection : "transparent"
        }
    }

    popup: Popup {
        y: control.height + 2
        width: control.width
        implicitHeight: Math.min(contentItem.implicitHeight + 8, 320)
        padding: 4
        contentItem: ListView {
            clip: true
            implicitHeight: contentHeight
            model: control.popup.visible ? control.delegateModel : null
            currentIndex: control.highlightedIndex
            ScrollIndicator.vertical: ScrollIndicator {}
        }
        background: Rectangle {
            radius: 6
            color: theme.darkBackground
            border.width: 1
            border.color: theme.accent
        }
    }
}
