import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import Play 1.0

Window {
    id: root
    width: Math.max(minimumWidth, settings.windowWidth)
    height: Math.max(minimumHeight, settings.windowHeight)
    // Remember the size the user leaves the window at (the setting itself is
    // debounced, so dragging does not hammer the disk).
    onWidthChanged: if (!isFullscreen() && visibility !== Window.Minimized) settings.windowWidth = width
    onHeightChanged: if (!isFullscreen() && visibility !== Window.Minimized) settings.windowHeight = height
    minimumWidth: 640
    minimumHeight: 420
    visible: true
    property bool playlistOpen: false
    // Fullscreen hides the chrome (bar, badge, buttons) after a couple of
    // seconds without mouse movement; any move brings it back.
    property bool chromeVisible: true
    color: "#08080b"
    title: video.source.length > 0 ? video.source.replace(/\\/g, "/").split("/").pop() : "play"

    function fmt(t) {
        if (!t || t < 0) t = 0
        const s = Math.floor(t % 60)
        const m = Math.floor(t / 60) % 60
        const h = Math.floor(t / 3600)
        const ss = s < 10 ? "0" + s : s
        return h > 0 ? h + ":" + (m < 10 ? "0" + m : m) + ":" + ss
                     : m + ":" + ss
    }

    function isFullscreen() {
        return root.visibility === Window.FullScreen
    }

    function toggleFullscreen() {
        root.visibility = isFullscreen() ? Window.Windowed : Window.FullScreen
    }

    // Shows the OSD pill and restarts its fade-out timer.
    function showOsd(text) {
        osdLabel.text = text
        osd.opacity = 1
        osdTimer.restart()
    }

    function wakeChrome() {
        root.chromeVisible = true
        hideTimer.restart()
    }

    onVisibilityChanged: {
        const wasFullscreen = root.lastVisibility === Window.FullScreen
        root.lastVisibility = root.visibility
        if (root.isFullscreen()) {
            root.wakeChrome()
            root.showOsd("全屏 · Esc 退出")
        } else if (wasFullscreen) {
            hideTimer.stop()
            root.chromeVisible = true
            root.showOsd("窗口模式")
        }
    }

    property int lastVisibility: Window.Windowed

    Timer {
        id: hideTimer
        interval: 2500
        onTriggered: {
            if (root.isFullscreen() && !barHover.hovered)
                root.chromeVisible = false
        }
    }

    VideoItem {
        id: video
        anchors.fill: parent
        // In fullscreen the bar floats over the picture instead of shrinking it.
        anchors.bottomMargin: root.isFullscreen() ? 0 : bottomBar.height
    }

    // Click/double-click and idle detection for the whole window. Declared
    // right after the video so the bar and the drawer stay on top of it.
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.LeftButton
        hoverEnabled: true
        onPressed: root.wakeChrome()
        onDoubleClicked: root.toggleFullscreen()
        onPositionChanged: root.wakeChrome()
    }

    function isSubtitle(u) {
        const s = u.toString().toLowerCase()
        return s.endsWith(".srt") || s.endsWith(".ass") || s.endsWith(".ssa")
               || s.endsWith(".vtt")
    }

    function playAt(row) {
        const p = playlist.pathAt(row)
        if (p.length === 0) return
        playlist.current = row
        video.source = p
    }

    function playNext() {
        const p = playlist.next()
        if (p.length > 0) video.source = p
    }

    function playPrevious() {
        const p = playlist.previous()
        if (p.length > 0) video.source = p
    }

    // Ctrl+R / the record button. Recording captures the playback itself, so it
    // only makes sense while a file is open; the file lands next to the source.
    function toggleRecording() {
        if (video.recording) {
            video.stopRecording()
            root.showOsd("录制已保存 · " + root.baseName(video.recordingFile))
            return
        }
        if (video.source.length === 0) {
            root.showOsd("没有可录制的视频")
            return
        }
        if (video.startRecording("")) {
            root.showOsd("● 录制中 · " + root.baseName(video.recordingFile))
        } else {
            root.showOsd("录制失败：" + video.errorString)
        }
    }

    function baseName(path) {
        return path.replace(/\\/g, "/").split("/").pop()
    }

    // --- screenshot (P5) ---
    // Ctrl+S: the current frame, through the recording chain (HDR tone mapped,
    // video resolution, no window chrome).
    function shot() {
        if (video.source.length === 0) return
        if (!video.saveScreenshot(""))
            root.showOsd("截图失败：" + video.errorString)
    }

    // --- clip export (P5.2) ---
    function fmtMark(t) {
        const s = Math.max(0, t)
        const m = Math.floor(s / 60)
        const r = (s - m * 60).toFixed(1)
        return m + ":" + (s - m * 60 < 10 ? "0" : "") + r
    }

    function markA() {
        if (video.source.length === 0) return
        video.markExportStart(-1)
        root.showOsd("A " + root.fmtMark(video.exportStart))
    }

    function markB() {
        if (video.source.length === 0) return
        video.markExportEnd(-1)
        root.showOsd("B " + root.fmtMark(video.exportEnd))
    }

    function exportClip() {
        if (video.exporting) {
            video.cancelExport()
            root.showOsd("已取消导出")
            return
        }
        if (video.source.length === 0) {
            root.showOsd("没有可导出的视频")
            return
        }
        const a = video.exportStart
        const b = video.exportEnd > 0 ? video.exportEnd : video.duration
        if (b - a < 0.2) {
            root.showOsd("先按 I 和 O 标出 A/B 点")
            return
        }
        root.exportShownPct = -1
        root.showOsd("导出 " + root.fmtMark(a) + " → " + root.fmtMark(b) + "（离线导出，比实时快；再按 Ctrl+E 取消）")
        if (!video.exportClip()) root.showOsd("导出失败：" + video.errorString)
    }

    // Drag & drop anywhere in the window.
    DropArea {
        anchors.fill: parent
        onDropped: (drop) => {
            if (!drop.hasUrls || drop.urls.length === 0)
                return
            const media = drop.urls.filter((u) => !isSubtitle(u))
            const subs = drop.urls.filter((u) => isSubtitle(u))
            if (media.length === 0) {
                if (subs.length > 0) video.openSubtitle(subs[0])
                return
            }
            playlist.add(media)
            if (subs.length > 0 && media.length === 1) video.openSubtitle(subs[0])
            video.open(media[0])
            playlist.setCurrentPath(video.source)
        }
    }

    // Playing to the end advances to the next file in the queue.
    Connections {
        target: video
        function onFinishedChanged() {
            if (video.finished && !video.loop && playlist.hasNext())
                root.playNext()
        }
    }

    // A locally chosen or recently used file becomes the current queue entry.
    Connections {
        target: video
        function onSourceChanged() {
            if (video.source.length > 0)
                playlist.setCurrentPath(video.source)
        }
        function onExportFinished(file, ok) {
            root.showOsd(ok ? "片段已导出 · " + root.baseName(file) : "导出未完成")
            if (root.startupExportQuit) Qt.quit()
        }
        function onExportProgressChanged() {
            if (!video.exporting || video.exportProgress <= 0) return
            // Every ~5% is enough; each OSD update is a repaint.
            const pct = Math.round(video.exportProgress * 20) * 5
            if (pct === root.exportShownPct) return
            root.exportShownPct = pct
            root.showOsd("导出中 " + pct + "%")
        }
        function onScreenshotSaved(file, ok) {
            root.showOsd(ok ? "截图已保存 · " + root.baseName(file) : "截图失败")
            if (root.startupShotQuit) Qt.quit()
        }
    }

    // Filename / decoder badge overlay.
    Rectangle {
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.margins: 16
        height: 30
        width: badge.implicitWidth + 24
        radius: 6
        color: "#99000000"
        visible: video.source.length > 0 && root.chromeVisible

        Text {
            id: badge
            anchors.centerIn: parent
            color: "#d8d8de"
            font.pixelSize: 12
            text: (video.hardwareDecoding ? "D3D11 硬解 · " : "软解 · ")
                  + video.source.replace(/\\/g, "/").split("/").pop()
        }
    }

    Text {
        anchors.centerIn: parent
        visible: video.source.length === 0
        color: "#4a4a55"
        font.pixelSize: 18
        text: "把视频拖进来，或点右下角「打开」"

        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.top: parent.bottom
            anchors.topMargin: 10
            color: "#7a2f2f"
            font.pixelSize: 13
            text: video.errorString
            visible: video.errorString.length > 0
        }
    }

    // Finished overlay: the play button restarts from the beginning.
    Rectangle {
        anchors.centerIn: parent
        anchors.verticalCenterOffset: -40
        visible: video.finished
        width: doneText.implicitWidth + 36
        height: 44
        radius: 22
        color: "#cc000000"

        Text {
            id: doneText
            anchors.centerIn: parent
            color: "#d8d8e6"
            font.pixelSize: 14
            text: "已播放完毕 · 点 ▶ 重播"
        }
    }

    Rectangle {
        id: bottomBar
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 74
        visible: root.chromeVisible
        color: "#121216"

        HoverHandler { id: barHover }

        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            height: 1
            color: "#22222a"
        }

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 18
            anchors.rightMargin: 18
            spacing: 14

            Button {
                text: video.playing ? "❚❚" : "▶"
                font.pixelSize: 15
                implicitWidth: 46
                implicitHeight: 36
                enabled: video.source.length > 0
                onClicked: video.togglePlay()
                background: Rectangle {
                    radius: 8
                    color: parent.hovered ? "#2a2a34" : "#1c1c24"
                    border.color: "#2e2e3a"
                }
                contentItem: Text {
                    text: parent.text
                    color: "#e6e6ec"
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    font: parent.font
                }
            }

            Text {
                text: fmt(video.position)
                color: "#9a9aa8"
                font.pixelSize: 12
                font.family: "Consolas"
                Layout.preferredWidth: 52
                horizontalAlignment: Text.AlignRight
            }

            Slider {
                id: seek
                Layout.fillWidth: true
                from: 0
                to: video.duration > 0 ? video.duration : 1
                value: video.position
                enabled: video.duration > 0
                onMoved: video.seekTo(value)

                background: Rectangle {
                    x: seek.leftPadding
                    y: seek.topPadding + seek.availableHeight / 2 - height / 2
                    width: seek.availableWidth
                    height: 4
                    radius: 2
                    color: "#2a2a34"

                    Rectangle {
                        width: seek.visualPosition * parent.width
                        height: parent.height
                        radius: 2
                        color: "#5b8cff"
                    }

                    // Clip export marks (I / O).
                    Repeater {
                        model: video.duration > 0
                               ? [{ t: video.exportStart, on: video.exportStart > 0 || video.exportEnd > 0 },
                                  { t: video.exportEnd, on: video.exportEnd > 0 }]
                               : []
                        delegate: Rectangle {
                            visible: modelData.on
                            width: 3
                            height: parent.height + 6
                            y: -3
                            radius: 1
                            color: "#ffd166"
                            x: Math.min(parent.width - width,
                                        Math.max(0, (modelData.t / video.duration) * parent.width - width / 2))
                        }
                    }
                }
                handle: Rectangle {
                    x: seek.leftPadding + seek.visualPosition * (seek.availableWidth - width)
                    y: seek.topPadding + seek.availableHeight / 2 - height / 2
                    width: 12
                    height: 12
                    radius: 6
                    color: seek.pressed ? "#8fb0ff" : "#e6ecff"
                }
            }

            Text {
                text: fmt(video.duration)
                color: "#6f6f7e"
                font.pixelSize: 12
                font.family: "Consolas"
                Layout.preferredWidth: 52
            }

            Button {
                id: loopButton
                text: "↻"
                checkable: true
                checked: video.loop
                implicitWidth: 40
                implicitHeight: 36
                enabled: video.source.length > 0
                onClicked: video.loop = checked
                background: Rectangle {
                    radius: 8
                    color: loopButton.checked ? "#2b3a5e" : (loopButton.hovered ? "#2a2a34" : "#1c1c24")
                    border.color: loopButton.checked ? "#5b8cff" : "#2e2e3a"
                }
                contentItem: Text {
                    text: parent.text
                    color: loopButton.checked ? "#cfe0ff" : "#e6e6ec"
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    font.pixelSize: 15
                }
            }

            Button {
                id: speedButton
                text: video.playbackSpeed + "x"
                implicitWidth: 52
                implicitHeight: 36
                enabled: video.source.length > 0
                onClicked: speedMenu.open()
                background: Rectangle {
                    radius: 8
                    color: speedButton.hovered ? "#2a2a34" : "#1c1c24"
                    border.color: video.playbackSpeed !== 1 ? "#5b8cff" : "#2e2e3a"
                }
                contentItem: Text {
                    text: speedButton.text
                    color: video.playbackSpeed !== 1 ? "#cfe0ff" : "#9a9aa8"
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    font.pixelSize: 12
                }

                Menu {
                    id: speedMenu
                    y: -height - 6
                    Instantiator {
                        model: [0.5, 0.75, 1.0, 1.25, 1.5, 2.0, 3.0, 4.0]
                        delegate: MenuItem {
                            text: modelData + "x"
                            checkable: true
                            checked: Math.abs(video.playbackSpeed - modelData) < 0.001
                            onTriggered: video.playbackSpeed = modelData
                        }
                        onObjectAdded: (i, obj) => speedMenu.insertItem(i, obj)
                        onObjectRemoved: (i, obj) => speedMenu.removeItem(obj)
                    }
                }
            }

            Button {
                id: audioButton
                visible: video.audioTracks.length > 1
                text: "音轨 " + (video.audioTracks.findIndex(t => t.current) + 1) + "/"
                      + video.audioTracks.length
                implicitWidth: 74
                implicitHeight: 36
                onClicked: audioMenu.open()

                background: Rectangle {
                    radius: 8
                    color: audioButton.hovered ? "#2a2a34" : "#1c1c24"
                    border.color: "#2e2e3a"
                }
                contentItem: Text {
                    text: audioButton.text
                    color: "#e6e6ec"
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    font.pixelSize: 12
                }

                Menu {
                    id: audioMenu
                    y: -height - 6
                    Instantiator {
                        model: video.audioTracks
                        delegate: MenuItem {
                            text: modelData.label
                            checkable: true
                            checked: modelData.current
                            onTriggered: video.selectAudioTrack(modelData.index)
                        }
                        onObjectAdded: (i, obj) => audioMenu.insertItem(i, obj)
                        onObjectRemoved: (i, obj) => audioMenu.removeItem(obj)
                    }
                }
            }

            Button {
                id: subButton
                text: "字幕"
                checkable: true
                checked: video.hasSubtitles && video.subtitlesEnabled
                implicitWidth: 56
                implicitHeight: 36
                onClicked: {
                    if (!video.hasSubtitles) {
                        checked = false
                        if (video.embeddedSubtitles.length > 0) subMenu.open()
                        else subDialog.open()
                    } else {
                        video.subtitlesEnabled = checked
                    }
                }
                background: Rectangle {
                    radius: 8
                    color: subButton.checked ? "#2b3a5e" : (subButton.hovered ? "#2a2a34" : "#1c1c24")
                    border.color: subButton.checked ? "#5b8cff" : "#2e2e3a"
                }
                contentItem: Text {
                    text: parent.text
                    color: subButton.checked ? "#cfe0ff" : "#9a9aa8"
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    font.pixelSize: 12
                }
            }

            Button {
                id: subMore
                visible: video.hasSubtitles || video.embeddedSubtitles.length > 0
                text: "▾"
                implicitWidth: 24
                implicitHeight: 36
                onClicked: subMenu.open()
                background: Rectangle {
                    radius: 8
                    color: subMore.hovered ? "#2a2a34" : "#1c1c24"
                    border.color: "#2e2e3a"
                }
                contentItem: Text {
                    text: parent.text
                    color: "#e6e6ec"
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    font.pixelSize: 11
                }

                Menu {
                    id: subMenu
                    y: -height - 6
                    // Embedded streams first, then sidecar files on disk.
                    Instantiator {
                        model: video.embeddedSubtitles
                        delegate: MenuItem {
                            text: modelData.label
                            enabled: modelData.text
                            checkable: true
                            checked: video.subtitleLabel === modelData.label
                            onTriggered: video.selectEmbeddedSubtitle(modelData.index)
                        }
                        onObjectAdded: (i, obj) => subMenu.insertItem(i, obj)
                        onObjectRemoved: (i, obj) => subMenu.removeItem(obj)
                    }
                    MenuSeparator {
                        visible: video.embeddedSubtitles.length > 0 && video.subtitleFiles.length > 0
                    }
                    Instantiator {
                        model: video.subtitleFiles
                        delegate: MenuItem {
                            text: modelData.replace(/\\/g, "/").split("/").pop()
                            checkable: true
                            checked: video.subtitleLabel === text
                            onTriggered: video.openSubtitle(modelData)
                        }
                        onObjectAdded: (i, obj) => subMenu.insertItem(i, obj)
                        onObjectRemoved: (i, obj) => subMenu.removeItem(obj)
                    }
                    MenuSeparator {}
                    MenuItem {
                        text: "打开字幕文件…"
                        onTriggered: subDialog.open()
                    }
                    MenuItem {
                        text: "关闭字幕"
                        enabled: video.hasSubtitles
                        onTriggered: video.subtitlesEnabled = false
                    }
                }
            }

            Button {
                id: scalerButton
                text: "画质"
                implicitWidth: 56
                implicitHeight: 36
                enabled: video.source.length > 0
                onClicked: scalerMenu.open()
                background: Rectangle {
                    radius: 8
                    color: scalerButton.hovered ? "#2a2a34" : "#1c1c24"
                    border.color: video.scaler !== "bilinear" ? "#5b8cff" : "#2e2e3a"
                }
                contentItem: Text {
                    text: scalerButton.text
                    color: video.scaler !== "bilinear" ? "#cfe0ff" : "#9a9aa8"
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    font.pixelSize: 12
                }

                // Only matters when the window does not match the video size;
                // at 1:1 the renderer keeps the plain present pass.
                Menu {
                    id: scalerMenu
                    y: -height - 6
                    Instantiator {
                        model: [
                            { value: "bilinear", label: "双线性 Bilinear（默认，最省）", name: "双线性" },
                            { value: "bicubic", label: "双三次 Bicubic", name: "双三次" },
                            { value: "lanczos3", label: "Lanczos3（最锐利）", name: "Lanczos3" }
                        ]
                        delegate: MenuItem {
                            text: modelData.label
                            checkable: true
                            checked: video.scaler === modelData.value
                            onTriggered: {
                                video.scaler = modelData.value
                                root.showOsd("缩放 " + modelData.name)
                            }
                        }
                        onObjectAdded: (i, obj) => scalerMenu.insertItem(i, obj)
                        onObjectRemoved: (i, obj) => scalerMenu.removeItem(obj)
                    }
                }
            }

            Button {
                id: recordButton
                text: video.exporting ? "导出中…" : (video.recording ? "■ 停止" : "● 录制")
                implicitWidth: video.exporting ? 76 : 68
                implicitHeight: 36
                enabled: video.source.length > 0 && !video.exporting
                onClicked: root.toggleRecording()
                background: Rectangle {
                    radius: 8
                    color: video.exporting ? "#3a3320"
                            : (video.recording ? "#5e2b2b" : (recordButton.hovered ? "#2a2a34" : "#1c1c24"))
                    border.color: video.exporting ? "#ffd166"
                                  : (video.recording ? "#ff6b6b" : "#2e2e3a")
                }
                contentItem: Text {
                    text: recordButton.text
                    color: video.exporting ? "#ffe9b0"
                           : (video.recording ? "#ffd2d2" : (recordButton.hovered ? "#e6e6ec" : "#9a9aa8"))
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    font.pixelSize: 12
                }
            }

            Button {
                id: openButton
                text: "打开"
                implicitWidth: 66
                implicitHeight: 36
                onClicked: fileDialog.open()
                background: Rectangle {
                    radius: 8
                    color: openButton.hovered ? "#2a2a34" : "#1c1c24"
                    border.color: "#2e2e3a"
                }
                contentItem: Text {
                    text: openButton.text
                    color: "#e6e6ec"
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    font.pixelSize: 13
                }
            }

            Button {
                id: recentMore
                visible: video.recentFiles.length > 0
                text: "▾"
                implicitWidth: 24
                implicitHeight: 36
                onClicked: recentMenu.open()
                background: Rectangle {
                    radius: 8
                    color: recentMore.hovered ? "#2a2a34" : "#1c1c24"
                    border.color: "#2e2e3a"
                }
                contentItem: Text {
                    text: parent.text
                    color: "#e6e6ec"
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    font.pixelSize: 11
                }

                Menu {
                    id: recentMenu
                    y: -height - 6
                    Instantiator {
                        model: video.recentFiles
                        delegate: MenuItem {
                            // "movie.mp4   03:12" so the resume point is visible.
                            text: modelData.name + (modelData.missing ? "  （已删除）"
                                  : (modelData.position > 0 ? "  " + fmt(modelData.position) : ""))
                            enabled: !modelData.missing
                            checkable: true
                            checked: modelData.path === video.source
                            onTriggered: video.source = modelData.path
                        }
                        onObjectAdded: (i, obj) => recentMenu.insertItem(i, obj)
                        onObjectRemoved: (i, obj) => recentMenu.removeItem(obj)
                    }
                    MenuSeparator {}
                    MenuItem {
                        text: "清除最近播放"
                        onTriggered: video.clearRecent()
                    }
                }
            }
        }
    }

    // Small square button used inside the queue drawer.
    component DrawerButton: Rectangle {
        id: db
        property string label
        signal clicked()
        implicitWidth: 30
        implicitHeight: 24
        radius: 6
        color: dbHover.hovered ? "#2a2a34" : "#1c1c24"
        border.color: "#2e2e3a"
        Text {
            anchors.centerIn: parent
            text: db.label
            color: db.enabled ? "#e6e6ec" : "#55555f"
            font.pixelSize: 12
        }
        HoverHandler { id: dbHover }
        TapHandler { onTapped: db.clicked() }
    }

    // Queue drawer: files can be added by drop, by "添加", or by opening one.
    Rectangle {
        id: playlistPanel
        anchors.top: parent.top
        anchors.bottom: bottomBar.top
        anchors.right: parent.right
        width: 300
        visible: root.playlistOpen
        color: "#f2121218"
        border.color: "#23232c"

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 10
            spacing: 8

            RowLayout {
                Layout.fillWidth: true
                spacing: 6
                Text {
                    text: "播放列表"
                    color: "#e6e6ec"
                    font.pixelSize: 13
                    font.bold: true
                }
                Text {
                    text: playlist.count > 0 ? "(" + playlist.count + ")" : ""
                    color: "#7a7a88"
                    font.pixelSize: 12
                }
                Item { Layout.fillWidth: true }
                DrawerButton {
                    label: "◀"
                    enabled: playlist.current > 0
                    onClicked: root.playPrevious()
                }
                DrawerButton {
                    label: "▶"
                    enabled: playlist.hasNext()
                    onClicked: root.playNext()
                }
                DrawerButton {
                    label: "×"
                    onClicked: root.playlistOpen = false
                }
            }

            ListView {
                id: queue
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                spacing: 2
                model: playlist
                ScrollBar.vertical: ScrollBar {}

                delegate: Rectangle {
                    id: queueRow
                    width: queue.width - 2
                    height: 44
                    radius: 6
                    color: model.current ? "#26324d"
                                         : (rowHover.hovered ? "#1f1f27" : "transparent")

                    Text {
                        anchors.left: parent.left
                        anchors.right: removeButton.left
                        anchors.top: parent.top
                        anchors.topMargin: 5
                        anchors.leftMargin: 8
                        anchors.rightMargin: 6
                        text: model.name
                        color: model.current ? "#cfe0ff" : "#d8d8e0"
                        font.pixelSize: 12
                        elide: Text.ElideMiddle
                    }
                    Text {
                        anchors.left: parent.left
                        anchors.right: removeButton.left
                        anchors.bottom: parent.bottom
                        anchors.bottomMargin: 5
                        anchors.leftMargin: 8
                        anchors.rightMargin: 6
                        text: model.dir
                        color: "#6f6f7e"
                        font.pixelSize: 10
                        elide: Text.ElideMiddle
                    }

                    DrawerButton {
                        id: removeButton
                        label: "×"
                        anchors.right: parent.right
                        anchors.rightMargin: 4
                        anchors.verticalCenter: parent.verticalCenter
                        visible: rowHover.hovered || model.current
                        onClicked: playlist.removeAt(index)
                    }

                    HoverHandler { id: rowHover }
                    TapHandler {
                        onTapped: root.playAt(index)
                    }
                }
            }

            Text {
                Layout.fillWidth: true
                visible: playlist.count === 0
                text: "把文件或文件夹拖进窗口即可加入队列"
                color: "#5f5f6c"
                font.pixelSize: 11
                wrapMode: Text.WordWrap
                horizontalAlignment: Text.AlignHCenter
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 6
                DrawerButton {
                    label: "添加"
                    implicitWidth: 52
                    onClicked: fileDialog.open()
                }
                Item { Layout.fillWidth: true }
                DrawerButton {
                    label: "清空"
                    implicitWidth: 52
                    enabled: playlist.count > 0
                    onClicked: playlist.clear()
                }
            }
        }
    }

    // Floating toggle for the drawer, top-right over the picture.
    Rectangle {
        id: queueToggle
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.topMargin: 16
        anchors.rightMargin: root.playlistOpen ? 316 : 16
        height: 30
        width: queueToggleLabel.implicitWidth + 20
        radius: 6
        visible: (video.source.length > 0 || playlist.count > 0 || root.playlistOpen)
                 && root.chromeVisible
        color: root.playlistOpen ? "#2b3a5e"
                                 : (queueToggleHover.hovered ? "#cc1f1f27" : "#99000000")
        border.color: root.playlistOpen ? "#5b8cff" : "#00000000"

        Text {
            id: queueToggleLabel
            anchors.centerIn: parent
            text: "列表" + (playlist.count > 0 ? " " + playlist.count : "")
            color: root.playlistOpen ? "#cfe0ff" : "#d8d8de"
            font.pixelSize: 12
        }
        HoverHandler { id: queueToggleHover }
        TapHandler {
            onTapped: root.playlistOpen = !root.playlistOpen
        }
    }

    // Transient status pill: speed, volume, seek steps, subtitles.
    Rectangle {
        id: osd
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: root.isFullscreen()
                            ? (root.chromeVisible ? bottomBar.height + 18 : 48)
                            : bottomBar.height + 18
        height: 34
        width: osdLabel.implicitWidth + 28
        radius: 17
        color: "#cc0d0d12"
        border.color: "#2e2e3a"
        opacity: 0
        visible: opacity > 0.01
        Behavior on opacity { NumberAnimation { duration: 140 } }

        Text {
            id: osdLabel
            anchors.centerIn: parent
            color: "#e6e6ec"
            font.pixelSize: 13
        }
    }

    Timer {
        id: osdTimer
        interval: 900
        onTriggered: osd.opacity = 0
    }

    FileDialog {
        id: fileDialog
        title: "选择视频"
        currentFolder: settings.lastDir.length > 0 ? "file:///" + settings.lastDir.replace(/\\/g, "/") : ""
        nameFilters: ["视频文件 (*.mp4 *.mkv *.mov *.avi *.webm *.ts *.flv *.m4v *.wmv)", "所有文件 (*)"]
        onAccepted: {
            const p = selectedFile.toString().replace(/^file:\/\/\//, "")
            const cut = p.lastIndexOf("/")
            settings.lastDir = cut > 0 ? p.substring(0, cut).replace(/\//g, "\\") : ""
            playlist.add([selectedFile])
            video.open(selectedFile)
        }
    }

    FileDialog {
        id: subDialog
        title: "选择字幕"
        nameFilters: ["字幕文件 (*.srt *.ass *.ssa *.vtt)", "所有文件 (*)"]
        onAccepted: video.openSubtitle(selectedFile)
    }

    Shortcut {
        sequence: "Space"
        onActivated: video.togglePlay()
    }
    Shortcut {
        sequence: "Left"
        onActivated: {
            const t = Math.max(0, video.position - 5)
            video.seekTo(t)
            root.showOsd("◀  " + fmt(t))
        }
    }
    Shortcut {
        sequence: "Right"
        onActivated: {
            const t = Math.min(video.duration, video.position + 5)
            video.seekTo(t)
            root.showOsd("▶  " + fmt(t))
        }
    }
    Shortcut {
        sequence: "L"
        onActivated: {
            video.loop = !video.loop
            root.showOsd(video.loop ? "循环播放 开" : "循环播放 关")
        }
    }
    Shortcut {
        sequence: "S"
        onActivated: {
            video.toggleSubtitles()
            root.showOsd((video.hasSubtitles && video.subtitlesEnabled) ? "字幕 开" : "字幕 关")
        }
    }
    Shortcut {
        sequence: "["
        onActivated: {
            video.playbackSpeed = Math.max(0.5, video.playbackSpeed - 0.25)
            root.showOsd("速度 " + video.playbackSpeed + "x")
        }
    }
    Shortcut {
        sequence: "]"
        onActivated: {
            video.playbackSpeed = Math.min(4, video.playbackSpeed + 0.25)
            root.showOsd("速度 " + video.playbackSpeed + "x")
        }
    }
    Shortcut {
        sequence: "Shift+Right"
        onActivated: root.playNext()
    }
    Shortcut {
        sequence: "Shift+Left"
        onActivated: root.playPrevious()
    }
    Shortcut {
        sequence: "F"
        onActivated: root.toggleFullscreen()
    }
    Shortcut {
        sequence: "Escape"
        onActivated: {
            if (root.isFullscreen()) root.visibility = Window.Windowed
            else if (root.playlistOpen) root.playlistOpen = false
        }
    }
    Shortcut {
        sequence: "Up"
        onActivated: {
            video.volume = Math.min(1, video.volume + 0.05)
            root.showOsd("音量 " + Math.round(video.volume * 100) + "%")
        }
    }
    Shortcut {
        sequence: "Down"
        onActivated: {
            video.volume = Math.max(0, video.volume - 0.05)
            root.showOsd("音量 " + Math.round(video.volume * 100) + "%")
        }
    }
    Shortcut {
        sequence: "Ctrl+R"
        onActivated: root.toggleRecording()
    }
    Shortcut {
        sequence: "I"
        onActivated: root.markA()
    }
    Shortcut {
        sequence: "O"
        onActivated: root.markB()
    }
    Shortcut {
        sequence: "Ctrl+E"
        onActivated: root.exportClip()
    }
    Shortcut {
        sequence: "Ctrl+S"
        onActivated: root.shot()
    }
    Shortcut {
        sequence: "M"
        onActivated: {
            video.toggleMute()
            root.showOsd(video.muted ? "静音" : "音量 " + Math.round(video.volume * 100) + "%")
        }
    }

    // Persist the knobs the user changes (the store debounces the writes).
    Connections {
        target: video
        function onVolumeChanged() { settings.volume = video.volume }
        function onMutedChanged() { settings.muted = video.muted }
        function onPlaybackSpeedChanged() { settings.speed = video.playbackSpeed }
        function onScalerChanged() { settings.scaler = video.scaler }
        function onLoopChanged() { settings.loop = video.loop }
    }

    Component.onCompleted: {
        // Stored preferences first: command-line flags below still win.
        video.volume = settings.volume
        if (settings.muted !== video.muted) video.toggleMute()
        video.playbackSpeed = settings.speed
        video.scaler = settings.scaler
        video.loop = settings.loop
        if (startupSpeed && startupSpeed > 0)
            video.playbackSpeed = startupSpeed
        if (startupScaler)
            video.scaler = startupScaler
        if (startupFiles && startupFiles.length > 0) {
            playlist.addPaths(startupFiles)
            video.source = startupFiles[0]
        }
        if (startupPlaylistOpen)
            root.playlistOpen = true
        if (startupFullscreen)
            root.visibility = Window.FullScreen
        // --seek: start somewhere other than 0 (testing, resuming by hand).
        if (startupSeek && startupSeek > 0)
            video.seekTo(startupSeek)
        // --record: capture from the first frame on (used by the smoke test).
        if (startupRecord && startupRecord.length > 0) {
            if (!video.startRecording(startupRecord))
                root.showOsd("录制失败：" + video.errorString)
        }
        // --export a:b: the item starts the clip export once the file is open.
        if (startupExportA >= 0 && startupExportB > startupExportA) {
            root.startupExportQuit = true
            video.exportWhenReady(startupExportA, startupExportB)
        }
        // --screenshot: save one frame after --shot-delay, then quit.
        if (startupScreenshot && startupScreenshot.length > 0) {
            root.startupShotQuit = true
            root.startupShotPath = startupScreenshot
            shotTimer.restart()
        }
    }

    Timer {
        id: shotTimer
        interval: startupShotDelay > 0 ? startupShotDelay : 2500
        onTriggered: video.saveScreenshot(root.startupShotPath)
    }

    // --export: the clip starts from the item (see exportWhenReady).
    property bool startupExportQuit: false
    property int exportShownPct: -1
    // --screenshot
    property bool startupShotQuit: false
    property string startupShotPath: ""
}
