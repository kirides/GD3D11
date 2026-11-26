#pragma once

/** Test automation: with -GAUTOMATION on the command line, keys written into <exe dir>/auto_keys.bin
    (memory-mapped, 256 bytes, index = DIK scancode, 0x80 = held) are merged into Gothic's DirectInput
    keyboard, because Gothic ignores injected SendInput keys. Driven by tools/gothic_auto.py. */
namespace AutomationInput {
    void Init();

    /** Main thread, once per frame: serves auto_cmd.txt (beam <waypoint>, pos, waypoints). No-op without -GAUTOMATION. */
    void Tick();
}
