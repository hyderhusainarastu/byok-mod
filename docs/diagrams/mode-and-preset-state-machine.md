# Display mode and preset-menu state machine

This diagram lays out two mode concepts side by side on purpose, because they are easy to
conflate and are not the same enum. The **link-level mode** (`SET_MODE`, values 0–7) is about
who owns the display bus and is a decision the connected host makes over the wire. The
**device-local mode** is about what the device itself is doing at any given moment, and it
keeps running with no host attached at all — a clock face does not stop ticking just because
nothing is plugged in. The device's own idle screen is a three-way cycle (clock face, stored
note text, or a blank panel) that the owner steers with the two rear buttons or that a host can
force directly; a preset menu overlays either side of the diagram, always remembering which
state it should hand control back to when it closes; and a transient hand-off state exists only
to let a last status frame go out honestly in the instant before the device restarts into stock
firmware — a path that is compiled out entirely in the currently shipped build.

```mermaid
stateDiagram-v2
    [*] --> HOST : boot completes, initial device-local mode is host/dashboard

    note left of HOST
        Two mode concepts exist and they are not the same enum. The link-level mode is about
        who owns the display bus and is set by the host. The device-local mode is about what
        the device is doing and outlives any one USB connection - a clock keeps ticking with
        no host attached.
    end note

    state HOST {
        [*] --> Displaying
        Displaying : host owns the display - drawing and frame commands are applied. Link-level values HOST value 1 and MIRROR value 2 both land here.
    }

    state "device's own screen" as IDLE_GROUP {
        CLOCK : large hour-minute time plus date and battery, driven by the RTC
        NOTE : the word-wrapped text most recently set by the host, laid out entirely by the device - 30 columns at the built-in glyph width; a payload over 200 bytes is rejected rather than truncated
        BLANK : panel cleared, backlight off

        CLOCK --> NOTE : UP
        NOTE --> BLANK : UP
        BLANK --> CLOCK : UP
        CLOCK --> BLANK : DOWN
        BLANK --> NOTE : DOWN
        NOTE --> CLOCK : DOWN
    }

    note right of IDLE_GROUP
        The UP and DOWN cycling above applies only while no host owns the display. While a
        host is active and the menu is closed, the same four buttons are reported to the
        host as events instead.
    end note

    HOST --> IDLE_GROUP : SET_MODE 0, IDLE - enters whichever of the three was last selected, NOT a hard-coded clock
    HOST --> CLOCK : SET_MODE 4
    HOST --> NOTE : SET_MODE 5
    HOST --> BLANK : SET_MODE 6
    HOST --> IDLE_GROUP : 30 s with no frame from a connected host, including no host having connected since boot
    IDLE_GROUP --> HOST : any frame arrives from any host - no SET_MODE needed first

    HOST --> MENU : EXECUTE held 1 s or more
    IDLE_GROUP --> MENU : EXECUTE held 1 s or more
    [*] --> MENU : automatically for 5 s at boot - skipped entirely if no preset is stored
    HOST --> MENU : SET_MODE 7

    state MENU {
        [*] --> Open
        Open : one preset name per row, current highlight drawn inverted. UP and DOWN move the highlight and wrap. EXECUTE confirms, closes, persists the index and fires a preset-changed event - including a re-confirm of the already-selected row. A 10 s timeout closes the menu and KEEPS the current selection - no persistence write and no event.
    }

    note right of MENU
        While the menu is open, drawing and refresh commands are still validated and
        acknowledged exactly as documented, but are not applied to the panel - so a host
        that keeps drawing cannot visually clobber the menu.
    end note

    MENU --> HOST : close, if the menu was opened from the host-owned state
    MENU --> IDLE_GROUP : close, restoring the exact idle submode that was showing

    SLEEP : link-level value 3 - bookkeeping only in the current firmware; the panel and backlight are not actually gated by it. Not the same thing as the BLANK idle submode.
    HOST --> SLEEP : SET_MODE 3
    SLEEP --> HOST : SET_MODE 1

    RETURN_TO_STOCK : transient state entered just before the restart that hands control to the stock application, so a status frame sent in the last instant can honestly report it
    HOST --> RETURN_TO_STOCK : the return-to-original command, or EXECUTE held 3 s at boot
    RETURN_TO_STOCK --> [*] : restart

    note right of RETURN_TO_STOCK
        Gated off in the shipped build - with otadata writes disabled the command is
        refused on the device rather than switching partitions, and the return to stock
        goes through the SD updater or esptool instead.
    end note

    note left of SLEEP
        PRESET SIDE-PANEL: up to 8 presets, each a 20-byte NUL-padded name, sent in menu
        order. List order IS wire order - reordering the list reorders the device's menu
        and changes what each index means. The list is cached on the device, so the menu
        works before any host connects, using whatever list was previously persisted. The
        selected index is reported twice, redundantly: as a dedicated event on each
        completed selection, and in bits 4-6 of the status flags byte, with bit 7 saying
        whether the menu is open. The host mirrors the selection by switching its live
        layout; it never restarts.
    end note

    note left of RETURN_TO_STOCK
        Persistence: the idle submode, the backlight index, the note text, the preset
        list and the selected index are stored under a namespace belonging to this
        firmware only. The stock namespaces in the same partition, which hold the Wi-Fi
        credentials, the Bluetooth pairing and the device token, are never opened, read
        or written, and the store is never erased.
    end note
```

## Facts and confidence

| Fact | Confidence | Source |
|---|---|---|
| Link-level `SET_MODE` values `HOST` (1) and `MIRROR` (2) both put the device in the host-owned display state | CONFIRMED | protocol.md |
| The device-local mode concept outlives any single USB connection (e.g. the clock keeps running with no host attached) | CONFIRMED | byok_modes.h |
| `SET_MODE 0` (`IDLE`) enters whichever of CLOCK / stored-note / blank was last selected, not a hard-coded clock | CONFIRMED | protocol.md |
| `SET_MODE` 4 / 5 / 6 force the clock / note / blank screen immediately and also select it for future `IDLE` entries | CONFIRMED | protocol.md |
| Rear UP/DOWN buttons cycle CLOCK → NOTE → BLANK (and reverse), active only while no host is connected | CONFIRMED | protocol.md |
| A `SET_NOTE` payload over 200 bytes is rejected outright rather than truncated | CONFIRMED | protocol.md |
| Note text is word-wrapped at 30 columns, matching the device's own panel width and glyph size | CONFIRMED | protocol.md |
| After 30 s with no frame from a connected host (including never having connected since boot), the device falls back to its own idle screen | CONFIRMED | protocol.md |
| Any frame from any host returns the device straight to host-owned mode, with no `SET_MODE` required first | CONFIRMED | protocol.md |
| Holding EXECUTE for 1 s or more opens the preset menu from either the host-owned state or an idle screen | CONFIRMED | protocol.md |
| The preset menu also opens automatically for 5 s at boot, but is skipped entirely if no preset is stored | CONFIRMED | protocol.md, app_main.c |
| `SET_MODE 7` opens the preset menu directly | CONFIRMED | protocol.md |
| In the menu, UP/DOWN move the highlight with wraparound; EXECUTE confirms, persists the index, and fires a preset-changed event even when re-confirming the already-selected row; a 10 s timeout closes the menu keeping the old selection with no write and no event | CONFIRMED | protocol.md |
| While the menu is open, drawing and refresh commands are still validated and acknowledged, but not applied to the panel | CONFIRMED | protocol.md, CHANGELOG.md |
| The menu remembers which state (host-owned, or a specific idle screen) to restore on close | CONFIRMED | byok_modes.h, protocol.md |
| Link-level `SLEEP` (value 3) is bookkeeping only in the current firmware; the panel and backlight are not actually gated by it, and it is distinct from the BLANK idle screen | CONFIRMED | protocol.md |
| A status frame sent just before the restart into stock firmware can honestly report the transient hand-off state | CONFIRMED | byok_modes.h |
| The return-to-stock action is reachable over the link or by holding EXECUTE for 3 s at boot | CONFIRMED | protocol.md |
| In the shipped build, the return-to-stock command is refused on the device (otadata writes disabled) rather than switching boot partitions | CONFIRMED | app_main.c |
| Up to 8 presets, each a 20-byte NUL-padded name, are sent and stored in menu order, which is also wire order | CONFIRMED | protocol.md |
| The preset list is cached on the device so the menu is populated before any host connects, using whatever list was previously persisted | CONFIRMED | protocol.md |
| The selected preset index is reported both as a dedicated event and in bits 4–6 of the status flags byte, with bit 7 indicating whether the menu is open | CONFIRMED | CHANGELOG.md |
| The host mirrors the device's preset selection by switching its live layout in place, never by restarting | CONFIRMED | host-tools.md |
| Idle-submode selection, backlight index, note text, preset list and selected preset index are all stored in a namespace private to this firmware, separate from the stock namespaces holding Wi-Fi credentials, Bluetooth pairing and the device token, which are never opened, read, written or erased | CONFIRMED | protocol.md |
