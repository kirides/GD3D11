#!/usr/bin/env python3
"""Drive a real Gothic process running GD3D11: launch, navigate menus, change settings, take screenshots.

Needs no DLL support. Input goes in through SendInput scancodes (Gothic reads DirectInput), engine hotkeys through
PostMessage (NUMPAD7 = screenshot, needs [General] AllowNumpadKeys=1, which this tool sets). Settings are applied by
editing UserSettings.ini before launch (and restored afterwards), so a settings change = one relaunch.

Examples (from the repo root):
  python tools/gothic_auto.py run --api D3D12 --wait 40 --steps "shot:menu"
  python tools/gothic_auto.py run --api Vulkan --set Display.VSync=0 --steps "key:enter;wait:5;shot:ingame"
  python tools/gothic_auto.py sweep --api D3D11,D3D12 --set-each "Shadows.EnableShadows=0|Shadows.EnableShadows=1" --steps "shot:x"

Fast start (verified in the ZenGin source, oGameManager/ooBert/oGame): there is NO command-line save loading. Use
  --new-game (-nomenu) [--world W.zen] [--time HH:MM]: skips the main menu, plays the intro (Esc skips), starts a new game.

Runtime changes, no restart, nothing saved (the game never writes UserSettings.ini; the tool loads a separate ini):
  set:Sec.Key=V[,Sec.Key=V]  (apply + reload shaders)  setfast:... (no reload)  reset (back to the launch settings)  reload (shaders)
Free camera (Gothic free-look, hero asleep; needs a loaded world):
  freecam:on|off  freecam_pos:x,y,z[,yawDeg,pitchDeg]  freecam_lookat:x,y,z[,dist]|NAME[,dist] (NAME = alias/waypoint/hero)  camget
  lookat only aims unless dist is given: then the camera flies along its line of sight to dist units (cm) from the target.

Step language (";" separated): beam:NAME|tp:ALIAS (teleport player to waypoint, then park him)  park:on|off  pos  waypoints (dump)
The hero is immortal while -GAUTOMATION runs; a beam parks him (asleep: no AI or physics, hits don't move him), park:off undoes it.
  wait:SEC  key:NAME[+NAME] (game input)  wmkey:NAME (GD3D11 hotkey: f11, num0)  hold:NAME:SEC  text:STR  click:X,Y  shot:LABEL (window capture, png)  eshot:LABEL (engine NUMPAD7 jpg)
  log:SUBSTRING (wait until Log.txt contains it, 120 s)  quit
Key names: enter esc space up down left right tab f1..f12 num0..num9 a..z 0..9 ctrl alt shift
Output: tools/auto_out/<run>/ (screenshots, Log.txt copy). Env: G2_SYSTEM_PATH / G1_SYSTEM_PATH (--game g1|g2).
"""
import argparse, mmap, ctypes, ctypes.wintypes as wt, os, shutil, subprocess, sys, time, itertools
from pathlib import Path

u32 = ctypes.windll.user32
ctypes.windll.user32.SetProcessDPIAware()  # physical pixels for window capture
k32 = ctypes.windll.kernel32

# Set 1 scancodes; arrows are extended keys.
SC = {'esc': 1, 'enter': 0x1C, 'space': 0x39, 'tab': 0x0F, 'ctrl': 0x1D, 'shift': 0x2A, 'alt': 0x38,
      'up': 0x48, 'down': 0x50, 'left': 0x4B, 'right': 0x4D, 'back': 0x0E,
      **{f'f{i}': 0x3A + i for i in range(1, 11)}, 'f11': 0x57, 'f12': 0x58,
      **{c: s for c, s in zip('qwertyuiop', range(0x10, 0x1A))},
      **{c: s for c, s in zip('asdfghjkl', range(0x1E, 0x27))},
      **{c: s for c, s in zip('zxcvbnm', range(0x2C, 0x33))},
      **{str(d): (0x0B if d == 0 else 1 + d) for d in range(10)},
      'num0': 0x52, 'num1': 0x4F, 'num2': 0x50, 'num3': 0x51, 'num4': 0x4B, 'num5': 0x4C, 'num6': 0x4D,
      'num7': 0x47, 'num8': 0x48, 'num9': 0x49}
EXT = {'up', 'down', 'left', 'right'}
WM_AUTOMATION = 0x8000 + 0x3D1  # WM_APP + 0x3D1, AutomationInput::WM_AUTOMATION
VK_NUM7 = 0x67
VK = {'f1': 0x70, 'f11': 0x7A, 'esc': 0x1B, 'enter': 0x0D, 'num7': 0x67, 'num0': 0x60, 'space': 0x20}
VK.update({f'f{i}': 0x6F + i for i in range(1, 13)})


class _KI(ctypes.Structure):
    _fields_ = [('wVk', wt.WORD), ('wScan', wt.WORD), ('dwFlags', wt.DWORD), ('time', wt.DWORD),
                ('dwExtraInfo', ctypes.c_size_t)]


class _MI(ctypes.Structure):
    _fields_ = [('dx', wt.LONG), ('dy', wt.LONG), ('mouseData', wt.DWORD), ('dwFlags', wt.DWORD),
                ('time', wt.DWORD), ('dwExtraInfo', ctypes.c_size_t)]


class _U(ctypes.Union):
    _fields_ = [('ki', _KI), ('mi', _MI)]


class INPUT(ctypes.Structure):
    _anonymous_ = ('u',)
    _fields_ = [('type', wt.DWORD), ('u', _U)]


def _send(inp):
    u32.SendInput(1, ctypes.byref(inp), ctypes.sizeof(INPUT))


def _key(name, up):
    i = INPUT(type=1)
    i.ki.wScan = SC[name]
    i.ki.dwFlags = 8 | (2 if up else 0) | (1 if name in EXT else 0)  # SCANCODE | KEYUP | EXTENDEDKEY
    _send(i)


def _ini_set(path, sec, key, val):
    k32.WritePrivateProfileStringW(sec, key, str(val), str(path))


WAYPOINTS = {}
_wp_file = Path(__file__).parent / 'gothic_waypoints.json'
if _wp_file.exists():
    import json
    WAYPOINTS = {k.lower(): v for k, v in json.loads(_wp_file.read_text()).items() if not k.startswith('_')}


class Game:
    def __init__(self, game='g2', run_name=None):
        env = 'G2_SYSTEM_PATH' if game == 'g2' else 'G1_SYSTEM_PATH'
        if env not in os.environ:
            sys.exit(f'{env} is not set')
        self.system = Path(os.environ[env])
        self.root = self.system.parent
        self.exe = self.system / ('Gothic2.exe' if game == 'g2' else 'Gothic.exe')
        self.ini = self.system / 'GD3D11' / 'UserSettings.ini'
        self.out = Path(__file__).parent / 'auto_out' / (run_name or time.strftime('%Y%m%d_%H%M%S'))
        self.out.mkdir(parents=True, exist_ok=True)
        self.proc = None
        self.hwnd = 0
        self._ini_backup = None
        self._log_offset = 0
        self.keys = None
        self._rt_overrides = {}

    # ---- settings -------------------------------------------------------------------------------------------
    def apply_settings(self, api, overrides):
        self._ini_backup = self.ini.read_bytes() if self.ini.exists() else None
        _ini_set(self.ini, 'General', 'AllowNumpadKeys', 1)
        if api:
            _ini_set(self.ini, 'Display', 'GraphicsAPI', api)
        for item in overrides:
            sk, v = item.split('=', 1)
            sec, key = sk.split('.', 1)
            _ini_set(self.ini, sec, key, v)

    def restore_settings(self):
        if self._ini_backup is not None:
            self.ini.write_bytes(self._ini_backup)
            self._ini_backup = None

    # ---- process / window -----------------------------------------------------------------------------------
    def launch(self, args=(), timeout=120):
        self._log_offset = self.log_path.stat().st_size if self.log_path.exists() else 0
        self.proc = subprocess.Popen([str(self.exe), '-GAUTOMATION', *args], cwd=str(self.root))
        t0 = time.time()
        while time.time() - t0 < timeout:
            if self.proc.poll() is not None:
                raise RuntimeError(f'game exited early (code {self.proc.returncode})')
            self.hwnd = self._find_window()
            if self.hwnd:
                self._attach_keys()
                return
            time.sleep(0.5)
        raise TimeoutError('no game window')

    def _attach_keys(self, timeout=30):
        """Map <System>/auto_keys.bin, the DLL's DirectInput key block (-GAUTOMATION); index = DIK code."""
        path = self.system / 'auto_keys.bin'
        if not path.exists() or path.stat().st_size < 256:
            path.write_bytes(bytes(256))
        self._keyfile = open(path, 'r+b')
        self.keys = mmap.mmap(self._keyfile.fileno(), 256)
        t0 = time.time()
        while time.time() - t0 < timeout:  
            if self.keys[0] == 0xA5:  # handshake written by AutomationInput::Init
                return
            time.sleep(0.5)
        print('  (DLL key block not confirmed; falling back to SendInput)')
        self.keys = None

    def _find_window(self):
        found = []

        def cb(h, _):
            pid = wt.DWORD()
            u32.GetWindowThreadProcessId(h, ctypes.byref(pid))
            if pid.value == self.proc.pid and u32.IsWindowVisible(h):
                r = wt.RECT()
                u32.GetWindowRect(h, ctypes.byref(r))
                found.append(((r.right - r.left) * (r.bottom - r.top), h))
            return True
        u32.EnumWindows(ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)(cb), 0)
        return max(found)[1] if found else 0  # largest visible window; early splash windows come and go

    def refresh_window(self):
        if not u32.IsWindow(self.hwnd):
            self.hwnd = self._find_window()

    def focus(self):
        for _ in range(5):
            self.refresh_window()
            u32.ShowWindow(self.hwnd, 9)
            u32.keybd_event(0x12, 0, 0, 0)  # an ALT press unlocks SetForegroundWindow
            u32.SetForegroundWindow(self.hwnd)
            u32.keybd_event(0x12, 0, 2, 0)
            time.sleep(0.25)
            if u32.GetForegroundWindow() == self.hwnd:
                return
        print('  warning: could not bring the game to the foreground')

    def focus_if_needed(self):
        self.refresh_window()
        if u32.GetForegroundWindow() != self.hwnd:
            self.focus()

    def alive(self):
        return self.proc is not None and self.proc.poll() is None

    def quit(self):
        if self.alive():
            self.proc.terminate()
            try:
                self.proc.wait(10)
            except subprocess.TimeoutExpired:
                self.proc.kill()

    # ---- log ------------------------------------------------------------------------------------------------
    @property
    def log_path(self):
        return self.system / 'Log.txt'

    def log_text(self):
        if not self.log_path.exists():
            return ''
        data = self.log_path.read_bytes()
        return (data[self._log_offset:] if len(data) >= self._log_offset else data).decode('utf-8', 'replace')

    def wait_log(self, needle, timeout=120):
        t0 = time.time()
        while time.time() - t0 < timeout:
            if needle in self.log_text():
                return True
            if not self.alive():
                raise RuntimeError('game died while waiting for log text: ' + needle)
            time.sleep(0.5)
        return False

    # ---- input ----------------------------------------------------------------------------------------------
    def key(self, combo, hold=0.08):
        names = combo.lower().split('+')
        for n in names:
            self._press(n, True)
        time.sleep(hold)
        for n in reversed(names):
            self._press(n, False)
        time.sleep(0.15)

    def hold(self, name, sec):
        self._press(name, True)
        time.sleep(sec)
        self._press(name, False)

    def _press(self, name, down):
        if down:
            self.focus_if_needed()  # DirectInput reads the keyboard in the foreground only
        if self.keys is not None:  # DirectInput code: scancode, +0x80 for extended keys
            self.keys[SC[name] | (0x80 if name in EXT else 0)] = 0x80 if down else 0  # mmap item assignment
        else:
            _key(name, not down)

    # ---- commands served by the DLL on the game's main thread (AutomationInput::Tick) --------------------------
    def cmd(self, text, timeout=15):
        """Send '<command> [args]' via auto_cmd.txt; returns the DLL's answer ('OK ...' / 'ERR ...')."""
        self._cmd_id = getattr(self, '_cmd_id', 0) + 1
        cid = str(self._cmd_id)
        res = self.system / 'auto_result.txt'
        res.unlink(missing_ok=True)
        tmp = self.system / 'auto_cmd.tmp'
        tmp.write_text(f'{cid} {text}' + chr(10))
        os.replace(tmp, self.system / 'auto_cmd.txt')  # atomic: the DLL never sees a half-written file
        self.refresh_window()
        u32.PostMessageW(self.hwnd, WM_AUTOMATION, 0, 0)  # serve it now on the main thread (works in menus too)
        t0 = time.time()
        while time.time() - t0 < timeout:
            if res.exists():
                line = res.read_text(errors='replace').strip()
                if line.split(' ', 1)[0] == cid:
                    return line.split(' ', 1)[1] if ' ' in line else ''
            time.sleep(0.1)
        return 'ERR timeout (world not loaded? the tick runs in-game only)'

    def beam(self, target, settle=2.5):
        """Teleport the player to a waypoint/freepoint/vob name, or an alias from tools/gothic_waypoints.json."""
        name = WAYPOINTS.get(target.lower(), target)
        r = self.cmd(f'beam {name}')
        print(f'  beam {name}: {r}')
        time.sleep(settle)  # let the world stream in and the camera settle
        return r.startswith('OK')

    # ---- runtime settings (never saved: a separate ini is loaded over the live settings) --------------------------
    def set_runtime(self, pairs, reload=True):
        """Apply 'Section.Key=Value' overrides on top of the launch ini without restarting or saving anything."""
        for item in pairs:
            sk, v = item.split('=', 1)
            self._rt_overrides[sk.strip()] = v.strip()
        return self._apply_runtime(reload)

    def reset_runtime(self, reload=True):
        self._rt_overrides.clear()
        return self._apply_runtime(reload)

    def _apply_runtime(self, reload):
        dst = self.system / 'GD3D11' / 'auto_settings.ini'
        shutil.copyfile(self.ini, dst)  # the launch ini: the game never saves, so the file is unchanged since launch
        for sk, v in self._rt_overrides.items():
            sec, key = sk.split('.', 1)
            _ini_set(dst, sec, key, v)
        r = self.cmd('applyini reload' if reload else 'applyini', timeout=120)
        print(f'  settings {dict(self._rt_overrides)}: {r}')
        return r.startswith('OK')

    def reload_shaders(self):
        r = self.cmd('reload', timeout=180)
        print(f'  reload shaders: {r}')
        return r.startswith('OK')

    # ---- free camera (Gothic free-look + hero asleep; use freecam:off to give the game back) -----------------
    def freecam(self, on):
        r = self.cmd('freecam on' if on else 'freecam off')
        print(f'  freecam {"on" if on else "off"}: {r}')
        return r.startswith('OK')

    def freecam_pos(self, args, settle=1.5):
        r = self.cmd('campos ' + ' '.join(a.strip() for a in args.split(',')))
        print(f'  campos: {r}')
        time.sleep(settle)
        return r.startswith('OK')

    def freecam_lookat(self, args, settle=1.5):
        """x,y,z[,dist] or NAME[,dist]; NAME = alias from gothic_waypoints.json, a waypoint, or hero."""
        t = [a.strip() for a in args.split(',')]
        try:
            float(t[0])
            target = ' '.join(t[:3]); rest = t[3:]
        except ValueError:
            target = '@' + WAYPOINTS.get(t[0].lower(), t[0]); rest = t[1:]
        r = self.cmd(f'lookat {target} {" ".join(rest)}'.strip())
        print(f'  lookat {target}: {r}')
        time.sleep(settle)
        return r.startswith('OK')

    def camget(self):
        r = self.cmd('camget')
        print(f'  camera: {r}  (x y z yaw pitch; feed to freecam_pos)')
        return r

    def wmkey(self, name):
        """Window-message key (WM_KEYDOWN/UP): reaches GD3D11's own hotkeys (F11 settings, NUMPAD0 shader reload)."""
        self.refresh_window()
        u32.PostMessageW(self.hwnd, 0x100, VK[name], 0)
        u32.PostMessageW(self.hwnd, 0x101, VK[name], 0)
        time.sleep(0.3)

    def text(self, s):
        for c in s.lower():
            self.key('space' if c == ' ' else c) if (c == ' ' or c in SC) else None

    def click(self, x, y):
        self.focus_if_needed()
        pt = wt.POINT(x, y)
        u32.ClientToScreen(self.hwnd, ctypes.byref(pt))
        u32.SetCursorPos(pt.x, pt.y)
        time.sleep(0.1)
        for f in (2, 4):  # LEFTDOWN, LEFTUP
            i = INPUT(type=0)
            i.mi.dwFlags = f
            _send(i)
            time.sleep(0.05)

    # ---- screenshots ----------------------------------------------------------------------------------------
    def grab(self, label):
        """Capture the presented client area (what the player sees, ImGui included) from the desktop."""
        from PIL import ImageGrab
        self.focus_if_needed()
        time.sleep(0.3)
        r = wt.RECT()
        u32.GetClientRect(self.hwnd, ctypes.byref(r))
        tl = wt.POINT(0, 0)
        u32.ClientToScreen(self.hwnd, ctypes.byref(tl))
        dst = self.out / f'{label}.png'
        ImageGrab.grab(bbox=(tl.x, tl.y, tl.x + r.right, tl.y + r.bottom), all_screens=True).save(dst)
        print(f'  shot -> {dst}')
        return dst

    def eshot(self, label, timeout=15):
        """Engine screenshot: NUMPAD7 -> engine writes <root>/system/screenshots/GD3D11_<ts>.jpg; move it into the run dir."""
        d = self.root / 'system' / 'screenshots'
        before = set(d.glob('*.jpg')) if d.exists() else set()
        self.refresh_window()
        u32.PostMessageW(self.hwnd, 0x100, VK_NUM7, 0)  # WM_KEYDOWN reaches GothicAPI::OnWindowMessage
        u32.PostMessageW(self.hwnd, 0x101, VK_NUM7, 0)
        t0 = time.time()
        while time.time() - t0 < timeout:
            new = (set(d.glob('*.jpg')) if d.exists() else set()) - before
            if new:
                time.sleep(0.5)  # let WIC finish
                dst = self.out / f'{label}.jpg'
                shutil.move(str(next(iter(new))), dst)
                print(f'  shot -> {dst}')
                return dst
            time.sleep(0.2)
        print(f'  shot {label}: no file appeared')
        return None

    # ---- scripting ------------------------------------------------------------------------------------------
    def steps(self, script):
        for raw in filter(None, (s.strip() for s in script.split(';'))):
            cmd, _, arg = raw.partition(':')
            if not self.alive() and cmd != 'quit':
                raise RuntimeError(f'game died before step "{raw}"')
            print(f'> {raw}')
            if cmd == 'wait': time.sleep(float(arg))
            elif cmd == 'key': self.key(arg)
            elif cmd in ('beam', 'tp'): print('  ok' if self.beam(arg) else '  FAILED')
            elif cmd == 'pos': print('  ' + self.cmd('pos'))
            elif cmd == 'park': print('  ' + self.cmd('park ' + (arg or 'on')))
            elif cmd == 'waypoints':
                r = self.cmd('waypoints')
                print('  ' + r)
                if r.startswith('OK'): shutil.copy(self.system / 'auto_waypoints.txt', self.out / 'waypoints.txt')
            elif cmd == 'set': print('  ok' if self.set_runtime(arg.split(',')) else '  FAILED')
            elif cmd == 'setfast': print('  ok' if self.set_runtime(arg.split(','), reload=False) else '  FAILED')
            elif cmd == 'reset': print('  ok' if self.reset_runtime() else '  FAILED')
            elif cmd == 'reload': print('  ok' if self.reload_shaders() else '  FAILED')
            elif cmd == 'freecam': print('  ok' if self.freecam(arg.lower() in ('on', '1', '')) else '  FAILED')
            elif cmd == 'freecam_pos': print('  ok' if self.freecam_pos(arg) else '  FAILED')
            elif cmd == 'freecam_lookat': print('  ok' if self.freecam_lookat(arg) else '  FAILED')
            elif cmd == 'camget': self.camget()
            elif cmd == 'wmkey': self.wmkey(arg.lower())
            elif cmd == 'hold': n, s = arg.split(':'); self.hold(n, float(s))
            elif cmd == 'text': self.text(arg)
            elif cmd == 'click': x, y = arg.split(','); self.click(int(x), int(y))
            elif cmd == 'shot': self.grab(arg or 'shot')
            elif cmd == 'eshot': self.eshot(arg or 'eshot')
            elif cmd == 'log': print('  found' if self.wait_log(arg) else '  TIMEOUT')
            elif cmd == 'quit': break
            else: sys.exit(f'unknown step {raw}')

    def finish(self):
        self.quit()
        if self.log_path.exists():
            (self.out / 'Log.txt').write_bytes(self.log_path.read_bytes()[self._log_offset:])
        (self.system / 'GD3D11' / 'auto_settings.ini').unlink(missing_ok=True)
        self.restore_settings()


def run_once(a, api, overrides, label=None):
    g = Game(a.game, label)
    try:
        g.apply_settings(api, overrides)
        print(f'== launch api={api or "(ini)"} set={overrides} out={g.out}')
        g.launch(a.args.split())
        time.sleep(a.wait)
        g.steps(a.steps)
    finally:
        g.finish()


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('mode', choices=['run', 'sweep'])
    p.add_argument('--game', default='g2', choices=['g1', 'g2'])
    p.add_argument('--api', default='', help='D3D11|D3D12|Vulkan (comma list in sweep mode)')
    p.add_argument('--set', action='append', default=[], help='Section.Key=Value for UserSettings.ini (repeatable)')
    p.add_argument('--set-each', default='', help='sweep: variants "A=1|A=2"; a variant may hold ","-joined sets')
    p.add_argument('--args', default='', help='extra Gothic command line, e.g. "-nomenu -zwindow"')
    p.add_argument('--new-game', action='store_true', help='-nomenu: skip MENU_MAIN, start a new game directly')
    p.add_argument('--world', default='', help='-3D:<world.zen> (with --new-game), no spaces, e.g. NewWorld\\NewWorld.zen')
    p.add_argument('--time', default='', help='-time:HH:MM initial in-game time (with --new-game)')
    p.add_argument('--wait', type=float, default=30, help='seconds after the window appears before steps run')
    p.add_argument('--steps', default='shot:menu')
    a = p.parse_args()
    extra = a.args.split()
    if a.new_game or a.world or a.time:
        extra.append('-nomenu')
    if a.world: extra.append('-3D:' + a.world)
    if a.time: extra.append('-time:' + a.time)
    a.args = ' '.join(extra)
    if a.mode == 'run':
        run_once(a, a.api, a.set)
    else:
        variants = a.set_each.split('|') if a.set_each else ['']
        for api, var in itertools.product(a.api.split(',') if a.api else [''], variants):
            ov = a.set + [v for v in var.split(',') if v]
            tag = f'{api or "default"}_{var.replace("=", "-").replace(",", "_")}'.strip('_')
            run_once(a, api, ov, time.strftime('%H%M%S_') + tag)


if __name__ == '__main__':
    main()
