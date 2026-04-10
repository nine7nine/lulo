const bridgeService = "org.ninez.LulodFocus";
const bridgePath = "/LulodFocus";
const bridgeInterface = "org.ninez.LulodFocus";

function activePid(window) {
    if (!window) {
        return 0;
    }
    if (window.deleted || window.desktopWindow || window.dock || window.popupWindow || window.outline || window.inputMethod) {
        return 0;
    }
    return window.pid || 0;
}

function report(window) {
    callDBus(bridgeService, bridgePath, bridgeInterface, "UpdateFocusedPid", activePid(window));
}

workspace.windowActivated.connect(report);
workspace.windowAdded.connect(function(window) {
    if (window === workspace.activeWindow) {
        report(window);
    }
});
report(workspace.activeWindow);
