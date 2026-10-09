#pragma once

/** Writes a minidump (GD3D11\Crashes\GD3D11-YYYYMMDD-HHMMSS.dmp) when the process crashes and then hands the
    crash on to the filter the game set. Players send the dump plus Log.txt; match against the build's DLL and PDB. */
namespace CrashDump {
    /** Call inside a Detours transaction, before the game's CRT sets its own filter. */
    void Install();
}
