#pragma once

/** Test automation: with -GAUTOMATION on the command line, keys written into <exe dir>/auto_keys.bin
    (memory-mapped, 256 bytes, index = DIK scancode, 0x80 = held) are merged into Gothic's DirectInput
    keyboard, because Gothic ignores injected SendInput keys. Commands (beam, freecam, lookat, applyini,
    reload, ...) come in through <exe dir>/auto_cmd.txt. Driven by tools/gothic_auto.py. */
namespace AutomationInput {
    /** Posted by the driver after writing auto_cmd.txt; GothicAPI::OnWindowMessage serves it on the main thread. */
    constexpr unsigned WM_AUTOMATION = 0x8000 + 0x3D1;  // WM_APP + 0x3D1

    void Init();

    /** Main thread: serves auto_cmd.txt. The per-frame call polls every few frames; force = serve now (WM_AUTOMATION). */
    void Tick( bool force = false );
}
