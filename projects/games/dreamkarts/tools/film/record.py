#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["obsws-python", "pyobjc-framework-Quartz"]
# ///
"""Record a DreamKarts film ELF in Flycast through OBS.

usage: OBS_WS_PASSWORD=... uv run tools/film/record.py <elf> <name>
           [--max-secs N] [--stop-on TEXT [--stop-after SECS] [--stop-nth N]] [--setup-only] [--no-record]
OBS must be running with its websocket server on port 4455.

OBS gets its own profile and scene collection ("DreamKarts Film") so the
user's own setup is left alone: a 1920x1080/60 canvas, one macOS window
capture of the Flycast window (title bar cropped, no cursor, 1440x1080
centred) and an application audio capture of Flycast only; no microphone.
DK FILM lines go to captures/film/runs/<name>.log with seconds since the
recording started, and the recording's path to <name>.path.
"""
import argparse, os, subprocess, sys, time
from pathlib import Path
import obsws_python as obs
import Quartz

PW = os.environ.get('OBS_WS_PASSWORD', '')
NAME = 'DreamKarts Film'
RAW = Path.home() / 'Movies' / 'DreamKarts Footage'
WORK = Path(__file__).resolve().parents[2] / 'captures' / 'film'
FLY = Path(__file__).resolve().parent / 'flycast-film.sh'

def client():
    return obs.ReqClient(host='localhost', port=4455, password=PW, timeout=10)

def setup(c):
    if c.get_record_status().output_active:
        sys.exit('OBS is already recording')
    if NAME not in c.get_profile_list().profiles:
        c.create_profile(NAME)
    if c.get_profile_list().current_profile_name != NAME:
        c.set_current_profile(NAME)
        time.sleep(1.5)
    c.set_video_settings(60, 1, 1920, 1080, 1920, 1080)
    RAW.mkdir(parents=True, exist_ok=True)
    for cat, key, val in [('Output', 'Mode', 'Simple'),
                          ('SimpleOutput', 'RecQuality', 'HQ'),
                          ('SimpleOutput', 'RecEncoder', 'apple_h264'),
                          ('SimpleOutput', 'RecFormat2', 'mkv'),
                          ('SimpleOutput', 'FilePath', str(RAW)),
                          ('SimpleOutput', 'RecAudioEncoder', 'aac'),
                          ('SimpleOutput', 'ABitrate', '320'),
                          ('Audio', 'SampleRate', '48000'),
                          ('Audio', 'ChannelSetup', 'Stereo')]:
        c.set_profile_parameter(cat, key, val)
    c.set_record_directory(str(RAW))
    # OBS builds its outputs when a profile loads: reload this one so the
    # recording uses the settings above.
    other = [p for p in c.get_profile_list().profiles if p != NAME]
    if not other:
        c.create_profile(NAME + ' (reload)')
        other = [NAME + ' (reload)']
    c.set_current_profile(other[0])
    time.sleep(1.5)
    c.set_current_profile(NAME)
    time.sleep(1.5)
    if NAME not in c.get_scene_collection_list().scene_collections:
        c.create_scene_collection(NAME)
        time.sleep(2)
    if c.get_scene_collection_list().current_scene_collection_name != NAME:
        c.set_current_scene_collection(NAME)
        time.sleep(2)
    scenes = [s['sceneName'] for s in c.get_scene_list().scenes]
    if 'Game' not in scenes:
        c.create_scene('Game')
    c.set_current_program_scene('Game')
    for s in scenes:
        if s != 'Game':
            c.remove_scene(s)
    names = {i['inputName']: i['inputKind'] for i in c.get_input_list().inputs}
    # No microphone or desktop audio: only the game.
    for n, kind in names.items():
        if kind in ('coreaudio_input_capture', 'coreaudio_output_capture'):
            c.remove_input(n)
    if 'Flycast Video' not in names:
        c.create_input('Game', 'Flycast Video', 'screen_capture',
                       {'type': 1, 'window': 0, 'show_cursor': False, 'show_empty_names': False}, True)
    if 'Flycast Audio' not in names:
        c.create_input('Game', 'Flycast Audio', 'sck_audio_capture',
                       {'type': 1, 'application': 'com.flyinghead.Flycast'}, True)
    c.set_input_settings('Flycast Audio', {'type': 1, 'application': 'com.flyinghead.Flycast'}, True)
    c.set_input_mute('Flycast Audio', False)
    c.set_input_volume('Flycast Audio', vol_db=0.0)
    print('special inputs', {k: v for k, v in vars(c.get_special_inputs()).items() if not k.startswith('_') and k != 'attrs'})

def flycast_window(stem):
    for w in Quartz.CGWindowListCopyWindowInfo(Quartz.kCGWindowListOptionAll, Quartz.kCGNullWindowID):
        if w.get('kCGWindowOwnerName') == 'Flycast' and stem in (w.get('kCGWindowName') or ''):
            b = w['kCGWindowBounds']
            if b['Height'] > 200:
                return w['kCGWindowNumber'], b
    return None, None

def point_capture(c, wid, bounds):
    # OBS's screen capture only knows windows from its last content scan;
    # listing the window property rescans.
    items = c.get_input_properties_list_property_items('Flycast Video', 'window').property_items
    print('window known to OBS:', any(i['itemValue'] == wid for i in items), flush=True)
    c.set_input_settings('Flycast Video', {'type': 1, 'window': wid, 'show_cursor': False}, True)
    time.sleep(1.5)
    scene_item = c.get_scene_item_id('Game', 'Flycast Video').scene_item_id
    # The window is 744x540 pt of content plus its title bar: the game's
    # 720x540 in the middle and two thin black bars holding macOS's rounded
    # bottom corners, all cropped away.
    title_px = int(round((bounds['Height'] - 540) * 2))
    side_px = int(round((bounds['Width'] * 2 - 1440) / 2))
    c.set_scene_item_transform('Game', scene_item, {
        # 4 px more off every edge: the capture has a thin dark window edge.
        'cropTop': title_px + 4, 'cropBottom': 4, 'cropLeft': side_px + 4, 'cropRight': side_px + 4,
        'positionX': 240.0, 'positionY': 0.0, 'alignment': 5, 'rotation': 0.0,
        'boundsType': 'OBS_BOUNDS_MAX_ONLY', 'boundsWidth': 1440.0, 'boundsHeight': 1080.0,
        'boundsAlignment': 0})
    t = c.get_scene_item_transform('Game', scene_item).scene_item_transform
    print('capture', {k: t[k] for k in ('sourceWidth', 'sourceHeight', 'cropTop', 'width', 'height', 'positionX')})
    return t

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('elf')
    ap.add_argument('name')
    ap.add_argument('--max-secs', type=float, default=600)
    ap.add_argument('--stop-on', default='')
    ap.add_argument('--stop-after', type=float, default=0, help='keep recording this long after --stop-on')
    ap.add_argument('--stop-nth', type=int, default=1, help='stop at the nth --stop-on line')
    ap.add_argument('--setup-only', action='store_true')
    ap.add_argument('--no-record', action='store_true')
    a = ap.parse_args()
    c = client()
    setup(c)
    if a.setup_only:
        return
    out = WORK / 'runs'
    out.mkdir(parents=True, exist_ok=True)
    serial = out / f'{a.name}.serial'
    proc = subprocess.Popen([str(FLY), str(Path(a.elf).resolve()), str(serial)])
    stem = Path(a.elf).stem
    wid = None
    for _ in range(100):
        wid, bounds = flycast_window(stem)
        if wid:
            break
        time.sleep(0.1)
    if not wid:
        proc.terminate()
        sys.exit('no Flycast window')
    t = point_capture(c, wid, bounds)
    if t['sourceWidth'] < 1000:
        time.sleep(2)
        t = point_capture(c, wid, bounds)
    shot = out / f'{a.name}-check.png'
    time.sleep(2)
    print('screenshot', flush=True)
    c.save_source_screenshot('Game', 'png', str(shot), 1920, 1080, -1)
    t0 = None
    if not a.no_record:
        print('starting record', flush=True)
        c.start_record()
        for _ in range(250):
            if c.get_record_status().output_active:
                break
            time.sleep(0.02)
        else:
            proc.terminate()
            sys.exit('recording did not start')
        st = c.get_record_status()
        t0 = time.time() - st.output_duration / 1000.0
        print('recording', flush=True)
    else:
        t0 = time.time()
    log = open(out / f'{a.name}.log', 'w')
    pos = 0
    stop_at = None
    seen = 0
    try:
        while stop_at is None or time.time() < stop_at:
            time.sleep(0.05)
            if proc.poll() is not None:
                print('Flycast exited', proc.returncode)
                break
            now = time.time() - t0
            with open(serial, 'rb') as f:
                f.seek(pos)
                data = f.read()
            nl = data.rfind(b'\n')
            if nl >= 0:
                chunk, pos = data[:nl + 1], pos + nl + 1
                for line in chunk.decode('latin-1').splitlines():
                    if line.startswith('DK FILM') or 'panic' in line or 'Unhandled' in line:
                        log.write(f'{now:9.3f} {line}\n')
                        log.flush()
                        if 'state' in line or 'error' in line or 'panic' in line or 'Unhandled' in line:
                            print(f'{now:8.1f} {line}', flush=True)
                        if a.stop_on and a.stop_on in line and stop_at is None:
                            seen += 1
                            if seen >= a.stop_nth:
                                stop_at = time.time() + a.stop_after
            if now > a.max_secs:
                break
    finally:
        if not a.no_record:
            r = c.stop_record()
            print('saved', r.output_path, flush=True)
            (out / f'{a.name}.path').write_text(r.output_path)
        time.sleep(1)
        proc.terminate()
        try:
            proc.wait(5)
        except subprocess.TimeoutExpired:
            proc.kill()

if __name__ == '__main__':
    main()
