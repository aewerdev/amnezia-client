import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import QtCore

import PageEnum 1.0
import Style 1.0

import "../Controls2"
import "../Controls2/TextTypes"
import "../Config"

PageType {
    id: root

    property string importFilePath: ""
    property string tokenFilePath: ""
    property string encodingValue: "auto"

    function displayFileName(filePath) {
        if (filePath === "") {
            return qsTr("Choose file")
        }
        var normalized = filePath.replace(/\\/g, "/")
        return normalized.substring(normalized.lastIndexOf("/") + 1)
    }

    function inspectSource() {
        if (importSourceTabs.currentIndex === 0) {
            UniversalSubscriptionController.inspectData(importText.textArea.text, root.encodingValue)
        } else if (importSourceTabs.currentIndex === 1) {
            UniversalSubscriptionController.inspectUrl(importUrl.textField.text, root.encodingValue)
        } else {
            UniversalSubscriptionController.inspectFile(root.importFilePath, root.encodingValue)
        }
    }

    function createToken() {
        var targetId = targetDeviceId.textField.text
        if (tokenSourceTabs.currentIndex === 0) {
            UniversalSubscriptionController.createToken(tokenText.textArea.text, targetId)
        } else if (tokenSourceTabs.currentIndex === 1) {
            UniversalSubscriptionController.createTokenFromUrl(tokenUrl.textField.text, targetId)
        } else {
            UniversalSubscriptionController.createTokenFromFile(root.tokenFilePath, targetId)
        }
    }

    function importSourceReady() {
        if (importSourceTabs.currentIndex === 0) {
            return importText.textArea.text.trim() !== ""
        }
        if (importSourceTabs.currentIndex === 1) {
            return importUrl.textField.text.trim() !== ""
        }
        return root.importFilePath !== ""
    }

    function tokenSourceReady() {
        if (tokenSourceTabs.currentIndex === 0) {
            return tokenText.textArea.text.trim() !== ""
        }
        if (tokenSourceTabs.currentIndex === 1) {
            return tokenUrl.textField.text.trim() !== ""
        }
        return root.tokenFilePath !== ""
    }

    Component.onDestruction: {
        UniversalSubscriptionController.clearInspection()
        UniversalSubscriptionController.clearGeneratedToken()
        PageController.showBusyIndicator(false)
    }

    Connections {
        target: UniversalSubscriptionController

        function onBusyChanged() {
            PageController.showBusyIndicator(UniversalSubscriptionController.busy)
        }

        function onOperationError(message) {
            PageController.showNotificationMessage(message)
        }

        function onImportCompleted(importedCount, failedCount) {
            if (failedCount > 0) {
                PageController.showNotificationMessage(
                            qsTr("Imported: %1, failed: %2").arg(importedCount).arg(failedCount))
                return
            }
            PageController.showNotificationMessage(qsTr("Configurations imported: %1").arg(importedCount))
            if (importedCount > 0) {
                PageController.goToPageHome()
            }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 0

            BackButtonType {
                Layout.topMargin: 20 + PageController.safeAreaTopMargin
            }

            Header2Type {
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16
                Layout.topMargin: 8
                Layout.bottomMargin: 12
                headerText: qsTr("Subscriptions")
            }
        }

        TabBar {
            id: mainTabs

            Layout.fillWidth: true
            background: Rectangle {
                color: AmneziaStyle.color.transparent
                Rectangle {
                    width: parent.width
                    height: 1
                    anchors.bottom: parent.bottom
                    color: AmneziaStyle.color.slateGray
                }
            }

            TabButtonType {
                text: qsTr("Import")
                isSelected: mainTabs.currentIndex === 0
            }

            TabButtonType {
                text: qsTr("Create 16x")
                isSelected: mainTabs.currentIndex === 1
            }
        }

        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: mainTabs.currentIndex

            Item {
                FlickableType {
                    anchors.fill: parent
                    contentHeight: importLayout.implicitHeight

                    ColumnLayout {
                        id: importLayout
                        width: parent.width
                        spacing: 0

                        LabelTextType {
                            Layout.fillWidth: true
                            Layout.leftMargin: 16
                            Layout.rightMargin: 16
                            Layout.topMargin: 20
                            text: qsTr("Source")
                        }

                        TabBar {
                            id: importSourceTabs

                            Layout.fillWidth: true
                            Layout.leftMargin: 16
                            Layout.rightMargin: 16
                            Layout.topMargin: 8

                            onCurrentIndexChanged: UniversalSubscriptionController.clearInspection()

                            background: Rectangle {
                                color: AmneziaStyle.color.transparent
                                Rectangle {
                                    width: parent.width
                                    height: 1
                                    anchors.bottom: parent.bottom
                                    color: AmneziaStyle.color.slateGray
                                }
                            }

                            TabButtonType {
                                text: qsTr("Text")
                                isSelected: importSourceTabs.currentIndex === 0
                            }
                            TabButtonType {
                                text: qsTr("URL")
                                isSelected: importSourceTabs.currentIndex === 1
                            }
                            TabButtonType {
                                text: qsTr("File")
                                isSelected: importSourceTabs.currentIndex === 2
                            }
                        }

                        StackLayout {
                            Layout.fillWidth: true
                            Layout.leftMargin: 16
                            Layout.rightMargin: 16
                            Layout.topMargin: 12
                            Layout.preferredHeight: importSourceTabs.currentIndex === 0 ? 148 : 72
                            currentIndex: importSourceTabs.currentIndex

                            TextAreaType {
                                id: importText
                                Layout.fillWidth: true
                                placeholderText: qsTr("Paste subscription text or a 16x token")
                                textArea.onTextChanged: UniversalSubscriptionController.clearInspection()
                            }

                            TextFieldWithHeaderType {
                                id: importUrl
                                Layout.fillWidth: true
                                headerText: qsTr("Subscription URL")
                                placeholderText: "https://"
                                textField.onTextChanged: UniversalSubscriptionController.clearInspection()
                            }

                            LabelWithButtonType {
                                Layout.fillWidth: true
                                text: root.displayFileName(root.importFilePath)
                                descriptionText: root.importFilePath === "" ? qsTr("Subscription file") : root.importFilePath
                                rightImageSource: "qrc:/images/controls/folder-open.svg"
                                clickedFunction: function() {
                                    var fileName = SystemController.getFileName(
                                                qsTr("Open subscription file"),
                                                qsTr("Subscription files (*.txt *.conf *.json *.vpn);;All files (*)"))
                                    if (fileName !== "") {
                                        root.importFilePath = fileName
                                        UniversalSubscriptionController.clearInspection()
                                    }
                                }
                            }
                        }

                        DropDownType {
                            id: encodingDropDown

                            Layout.fillWidth: true
                            Layout.leftMargin: 16
                            Layout.rightMargin: 16
                            Layout.topMargin: 16

                            text: qsTr("Auto detect")
                            descriptionText: qsTr("Encoding")
                            headerText: qsTr("Encoding")
                            drawerParent: root
                            fitContent: true

                            listView: ListViewWithRadioButtonType {
                                id: encodingList
                                rootWidth: root.width

                                model: ListModel {
                                    ListElement { name: qsTr("Auto detect"); value: "auto" }
                                    ListElement { name: qsTr("Plain text"); value: "plain" }
                                    ListElement { name: qsTr("Base64"); value: "base64" }
                                    ListElement { name: qsTr("Hex"); value: "hex" }
                                }

                                clickedFunction: function() {
                                    var selected = model.get(selectedIndex)
                                    encodingDropDown.text = selected.name
                                    root.encodingValue = selected.value
                                    encodingDropDown.closeTriggered()
                                    UniversalSubscriptionController.clearInspection()
                                }
                            }
                        }

                        BasicButtonType {
                            Layout.fillWidth: true
                            Layout.leftMargin: 16
                            Layout.rightMargin: 16
                            Layout.topMargin: 16

                            enabled: root.importSourceReady() && !UniversalSubscriptionController.busy
                            text: qsTr("Check subscription")
                            leftImageSource: "qrc:/images/controls/search.svg"
                            clickedFunc: root.inspectSource
                        }

                        CaptionTextType {
                            Layout.fillWidth: true
                            Layout.leftMargin: 16
                            Layout.rightMargin: 16
                            Layout.topMargin: 12
                            visible: UniversalSubscriptionController.errorText !== ""
                            color: AmneziaStyle.color.vibrantRed
                            wrapMode: Text.WordWrap
                            text: UniversalSubscriptionController.errorText
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            Layout.topMargin: 20
                            visible: UniversalSubscriptionController.hasInspection
                            spacing: 0

                            RowLayout {
                                Layout.fillWidth: true
                                Layout.leftMargin: 16
                                Layout.rightMargin: 16

                                LabelTextType {
                                    Layout.fillWidth: true
                                    text: qsTr("Found: %1").arg(UniversalSubscriptionController.entryCount)
                                }

                                CaptionTextType {
                                    text: qsTr("Valid: %1").arg(UniversalSubscriptionController.validCount)
                                    color: AmneziaStyle.color.paleGray
                                }

                                CaptionTextType {
                                    text: qsTr("Invalid: %1").arg(UniversalSubscriptionController.invalidCount)
                                    color: UniversalSubscriptionController.invalidCount > 0
                                           ? AmneziaStyle.color.vibrantRed : AmneziaStyle.color.mutedGray
                                }
                            }

                            CaptionTextType {
                                Layout.fillWidth: true
                                Layout.leftMargin: 16
                                Layout.rightMargin: 16
                                Layout.topMargin: 4
                                text: UniversalSubscriptionController.deviceBound
                                      ? qsTr("16x for this device, decoded as %1").arg(UniversalSubscriptionController.detectedEncoding)
                                      : qsTr("Encoding: %1").arg(UniversalSubscriptionController.detectedEncoding)
                                color: AmneziaStyle.color.mutedGray
                            }

                            DividerType {
                                Layout.topMargin: 12
                            }

                            Repeater {
                                model: UniversalSubscriptionController.entries

                                delegate: ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 0

                                    RowLayout {
                                        Layout.fillWidth: true
                                        Layout.leftMargin: 16
                                        Layout.rightMargin: 16
                                        Layout.topMargin: 12
                                        Layout.bottomMargin: modelData.message === "" ? 12 : 4

                                        Image {
                                            Layout.preferredWidth: 20
                                            Layout.preferredHeight: 20
                                            source: modelData.valid
                                                    ? (modelData.warning
                                                       ? "qrc:/images/controls/alert-circle.svg"
                                                       : "qrc:/images/controls/check.svg")
                                                    : "qrc:/images/controls/x-circle.svg"
                                        }

                                        ListItemTitleType {
                                            Layout.fillWidth: true
                                            text: "#" + modelData.index + "  " + modelData.kind.toUpperCase()
                                        }
                                    }

                                    CaptionTextType {
                                        Layout.fillWidth: true
                                        Layout.leftMargin: 52
                                        Layout.rightMargin: 16
                                        Layout.bottomMargin: 12
                                        visible: modelData.message !== ""
                                        color: modelData.valid ? AmneziaStyle.color.goldenApricot
                                                               : AmneziaStyle.color.vibrantRed
                                        wrapMode: Text.WordWrap
                                        text: modelData.message
                                    }

                                    DividerType { }
                                }
                            }

                            SwitcherType {
                                id: partialImport

                                Layout.fillWidth: true
                                Layout.leftMargin: 16
                                Layout.rightMargin: 16
                                Layout.topMargin: 16
                                visible: UniversalSubscriptionController.invalidCount > 0
                                text: qsTr("Import valid entries only")
                                descriptionText: qsTr("Invalid entries will be skipped")
                            }

                            BasicButtonType {
                                Layout.fillWidth: true
                                Layout.leftMargin: 16
                                Layout.rightMargin: 16
                                Layout.topMargin: 16
                                Layout.bottomMargin: 24

                                enabled: UniversalSubscriptionController.validCount > 0
                                         && (UniversalSubscriptionController.invalidCount === 0 || partialImport.checked)
                                         && !UniversalSubscriptionController.busy
                                text: qsTr("Import configurations")
                                leftImageSource: "qrc:/images/controls/download.svg"
                                clickedFunc: function() {
                                    UniversalSubscriptionController.importInspected(partialImport.checked)
                                }
                            }

                        }
                    }
                }
            }

            Item {
                FlickableType {
                    anchors.fill: parent
                    contentHeight: tokenLayout.implicitHeight

                    ColumnLayout {
                        id: tokenLayout
                        width: parent.width
                        spacing: 0

                        TextFieldWithHeaderType {
                            Layout.fillWidth: true
                            Layout.leftMargin: 16
                            Layout.rightMargin: 16
                            Layout.topMargin: 20

                            headerText: qsTr("This device ID")
                            textField.text: UniversalSubscriptionController.deviceId
                            textFieldEditable: false
                            buttonImageSource: "qrc:/images/controls/copy.svg"
                            clickedFunc: function() {
                                GC.copyToClipBoard(UniversalSubscriptionController.deviceId)
                                PageController.showNotificationMessage(qsTr("Copied"))
                            }
                        }

                        TextFieldWithHeaderType {
                            id: targetDeviceId

                            Layout.fillWidth: true
                            Layout.leftMargin: 16
                            Layout.rightMargin: 16
                            Layout.topMargin: 12

                            headerText: qsTr("Target 32x device ID")
                            placeholderText: "32x..."
                            buttonText: qsTr("Paste")
                            clickedFunc: function() {
                                textField.text = ""
                                textField.paste()
                            }
                            textField.onTextChanged: UniversalSubscriptionController.clearGeneratedToken()
                        }

                        LabelTextType {
                            Layout.fillWidth: true
                            Layout.leftMargin: 16
                            Layout.rightMargin: 16
                            Layout.topMargin: 20
                            text: qsTr("Payload source")
                        }

                        TabBar {
                            id: tokenSourceTabs

                            Layout.fillWidth: true
                            Layout.leftMargin: 16
                            Layout.rightMargin: 16
                            Layout.topMargin: 8

                            onCurrentIndexChanged: UniversalSubscriptionController.clearGeneratedToken()

                            background: Rectangle {
                                color: AmneziaStyle.color.transparent
                                Rectangle {
                                    width: parent.width
                                    height: 1
                                    anchors.bottom: parent.bottom
                                    color: AmneziaStyle.color.slateGray
                                }
                            }

                            TabButtonType {
                                text: qsTr("Text")
                                isSelected: tokenSourceTabs.currentIndex === 0
                            }
                            TabButtonType {
                                text: qsTr("URL")
                                isSelected: tokenSourceTabs.currentIndex === 1
                            }
                            TabButtonType {
                                text: qsTr("File")
                                isSelected: tokenSourceTabs.currentIndex === 2
                            }
                        }

                        StackLayout {
                            Layout.fillWidth: true
                            Layout.leftMargin: 16
                            Layout.rightMargin: 16
                            Layout.topMargin: 12
                            Layout.preferredHeight: tokenSourceTabs.currentIndex === 0 ? 148 : 72
                            currentIndex: tokenSourceTabs.currentIndex

                            TextAreaType {
                                id: tokenText
                                Layout.fillWidth: true
                                placeholderText: qsTr("Payload to encrypt")
                                textArea.onTextChanged: UniversalSubscriptionController.clearGeneratedToken()
                            }

                            TextFieldWithHeaderType {
                                id: tokenUrl
                                Layout.fillWidth: true
                                headerText: qsTr("Payload URL")
                                placeholderText: "https://"
                                textField.onTextChanged: UniversalSubscriptionController.clearGeneratedToken()
                            }

                            LabelWithButtonType {
                                Layout.fillWidth: true
                                text: root.displayFileName(root.tokenFilePath)
                                descriptionText: root.tokenFilePath === "" ? qsTr("Payload file") : root.tokenFilePath
                                rightImageSource: "qrc:/images/controls/folder-open.svg"
                                clickedFunction: function() {
                                    var fileName = SystemController.getFileName(
                                                qsTr("Open payload file"),
                                                qsTr("Subscription files (*.txt *.conf *.json *.vpn);;All files (*)"))
                                    if (fileName !== "") {
                                        root.tokenFilePath = fileName
                                        UniversalSubscriptionController.clearGeneratedToken()
                                    }
                                }
                            }
                        }

                        BasicButtonType {
                            Layout.fillWidth: true
                            Layout.leftMargin: 16
                            Layout.rightMargin: 16
                            Layout.topMargin: 16

                            enabled: targetDeviceId.textField.text.trim() !== ""
                                     && root.tokenSourceReady()
                                     && !UniversalSubscriptionController.busy
                            text: qsTr("Create 16x token")
                            leftImageSource: "qrc:/images/controls/file-check-2.svg"
                            clickedFunc: root.createToken
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            Layout.topMargin: 20
                            Layout.bottomMargin: 24
                            visible: UniversalSubscriptionController.generatedToken !== ""
                            spacing: 0

                            LabelTextType {
                                Layout.fillWidth: true
                                Layout.leftMargin: 16
                                Layout.rightMargin: 16
                                text: qsTr("16x token")
                            }

                            TextAreaType {
                                Layout.fillWidth: true
                                Layout.leftMargin: 16
                                Layout.rightMargin: 16
                                Layout.topMargin: 8
                                textArea.readOnly: true
                                textArea.text: UniversalSubscriptionController.generatedToken
                            }

                            BasicButtonType {
                                Layout.fillWidth: true
                                Layout.leftMargin: 16
                                Layout.rightMargin: 16
                                Layout.topMargin: 12
                                text: qsTr("Copy token")
                                leftImageSource: "qrc:/images/controls/copy.svg"
                                clickedFunc: function() {
                                    GC.copyToClipBoard(UniversalSubscriptionController.generatedToken)
                                    PageController.showNotificationMessage(qsTr("Copied"))
                                }
                            }

                            BasicButtonType {
                                Layout.fillWidth: true
                                Layout.leftMargin: 16
                                Layout.rightMargin: 16
                                Layout.topMargin: 4

                                defaultColor: AmneziaStyle.color.transparent
                                hoveredColor: AmneziaStyle.color.translucentWhite
                                pressedColor: AmneziaStyle.color.sheerWhite
                                textColor: AmneziaStyle.color.paleGray
                                borderWidth: 1

                                text: qsTr("Save token")
                                leftImageSource: "qrc:/images/controls/share-2.svg"
                                clickedFunc: function() {
                                    var fileName = SystemController.getFileName(
                                                qsTr("Save 16x token"),
                                                qsTr("16x subscriptions (*.16x);;Text files (*.txt)"),
                                                StandardPaths.standardLocations(StandardPaths.DocumentsLocation)
                                                + "/subscription",
                                                true,
                                                ".16x")
                                    if (fileName !== ""
                                            && UniversalSubscriptionController.saveGeneratedToken(fileName)) {
                                        PageController.showNotificationMessage(qsTr("Token saved"))
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
