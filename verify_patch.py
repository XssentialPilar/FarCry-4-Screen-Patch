import argparse
import ctypes
import hashlib
import math
import os
import struct
import pefile

DEFAULT_BIN_DIR = r"C:\Program Files (x86)\Steam\steamapps\common\Far Cry 4\bin"

TEST_ASPECT_RATIOS = [
    ("4:3", 1280, 960),
    ("16:10", 1920, 1200),
    ("3:2 (1.50:1)", 1500, 1000),
    ("5:4", 1280, 1024),
    ("4:5", 1024, 1280),
    ("2:1 (2.00:1)", 2000, 1000),
    ("21:9 (2560x1080)", 2560, 1080),
    ("2.375:1", 2375, 1000),
    ("1.85:1", 1850, 1000),
    ("2.35:1", 2350, 1000),
    ("2.39:1", 2390, 1000),
    ("2.40:1", 2400, 1000),
    ("2.76:1", 2760, 1000),
    ("9:16", 1080, 1920),
    ("18:9", 2160, 1080),
    ("19.5:1", 3900, 200),
    ("19.5:9", 2340, 1080),
    ("20:9", 2400, 1080),
]


class UiLayoutIO(ctypes.Structure):
    _fields_ = [
        ("vpW", ctypes.c_double),
        ("vpH", ctypes.c_double),
        ("safeL", ctypes.c_double),
        ("safeT", ctypes.c_double),
        ("safeR", ctypes.c_double),
        ("safeB", ctypes.c_double),
        ("scaleX", ctypes.c_double),
        ("scaleY", ctypes.c_double),
        ("cachedW", ctypes.c_double),
        ("cachedH", ctypes.c_double),
        ("one", ctypes.c_double),
        ("mode", ctypes.c_int32),
        ("pad", ctypes.c_int32),
    ]


class MockDisplayViewportInfo(ctypes.Structure):
    _fields_ = [
        ("pad0", ctypes.c_ubyte * 3),
        ("bMultiMonSplit", ctypes.c_uint8),
        ("pad1", ctypes.c_uint32),
        ("width", ctypes.c_uint32),
        ("height", ctypes.c_uint32),
        ("aspect", ctypes.c_float),
        ("pad2", ctypes.c_uint32),
        ("rectsPtr", ctypes.c_uint64),
        ("rectCount", ctypes.c_uint32),
        ("pad3", ctypes.c_uint32),
    ]


def hash_text_section(h_fc64):
    text_buf = (ctypes.c_ubyte * 0x25e2714).from_address(h_fc64 + 0x1000)
    return hashlib.sha256(memoryview(text_buf)).hexdigest()


def main():
    parser = argparse.ArgumentParser(description="Verify Far Cry 4 Screen Patch against unpacked FC64.dll")
    parser.add_argument(
        "--bin-dir",
        default=os.environ.get("FC4_BIN_DIR", DEFAULT_BIN_DIR),
        help="Path to the Far Cry 4 bin directory containing FC64.dll and dinput8.dll",
    )
    args = parser.parse_args()
    bin_dir = os.path.abspath(args.bin_dir)
    os.chdir(bin_dir)

    proxy_path = os.path.join(bin_dir, "dinput8.dll")
    pe = pefile.PE(proxy_path)
    assert pe.FILE_HEADER.Machine == 0x8664
    imports = [e.dll.decode().lower() for e in pe.DIRECTORY_ENTRY_IMPORT]
    for imp in imports:
        assert imp in ("kernel32.dll", "user32.dll") or imp.startswith("api-ms-win-crt-"), f"Unexpected import: {imp}"
    print(f"[PASS] PE check clean (x64, system-only imports: {len(imports)} DLLs)")

    k32 = ctypes.windll.kernel32
    k32.LoadLibraryW.restype = ctypes.c_void_p

    h_proxy = k32.LoadLibraryW(proxy_path)
    assert h_proxy, f"Failed to load {proxy_path}"
    proxy = ctypes.WinDLL(proxy_path, handle=h_proxy)

    proxy.DllCanUnloadNow.restype = ctypes.c_long
    hr = proxy.DllCanUnloadNow()
    assert hr in (0, 1), f"Unexpected DllCanUnloadNow HRESULT: {hr}"

    proxy.GetdfDIJoystick.restype = ctypes.c_void_p
    joy_fmt = proxy.GetdfDIJoystick()
    assert joy_fmt, "GetdfDIJoystick returned NULL"
    didf_dwsize = struct.unpack("<I", bytes((ctypes.c_ubyte * 4).from_address(joy_fmt)))[0]
    assert didf_dwsize in (24, 32)
    print(f"[PASS] dinput8 proxy forwarding verified (DllCanUnloadNow={hr}, dwSize={didf_dwsize})")

    h_fc64 = k32.LoadLibraryW("FC64.dll")
    assert h_fc64, "Failed to load FC64.dll"
    hash_before = hash_text_section(h_fc64)
    print(f"[PASS] FC64.dll unpacked at {hex(h_fc64)}, .text SHA-256={hash_before[:16]}...")

    proxy.FC4_AspectFix_TestApply.argtypes = [ctypes.c_void_p]
    proxy.FC4_AspectFix_TestApply.restype = ctypes.c_int
    resolved = proxy.FC4_AspectFix_TestApply(h_fc64)
    assert resolved == 10, f"Expected 10 resolved symbols, got {resolved}"

    hash_after = hash_text_section(h_fc64)
    assert hash_before == hash_after, "FC64.dll .text was modified!"
    print("[PASS] 10/10 symbols resolved uniquely; .text SHA-256 100% UNCHANGED (0 bytes edited)")

    log_path = os.path.join(bin_dir, "FC4_AspectFix.log")
    with open(log_path, "r", encoding="utf-8") as f:
        log_text = f.read()
    assert "[ERROR]" not in log_text, f"Errors in log:\n{log_text}"
    assert "[WARN]" not in log_text, f"Warnings in log:\n{log_text}"
    assert "[SUMMARY] Resolved 10/10 engine symbols" in log_text
    print("[PASS] FC4_AspectFix.log clean (zero warnings/errors)")

    fn_ui = proxy.FC4_AspectFix_ComputeUiLayout
    fn_ui.argtypes = [ctypes.POINTER(UiLayoutIO)]
    fn_ui.restype = None

    fn_norm = proxy.FC4_AspectFix_NormalizeViewport
    fn_norm.argtypes = [
        ctypes.POINTER(MockDisplayViewportInfo),
        ctypes.POINTER(ctypes.c_uint32),
        ctypes.POINTER(ctypes.c_uint32),
        ctypes.POINTER(ctypes.c_float),
    ]
    fn_norm.restype = ctypes.c_bool

    rc_buf = (ctypes.c_uint8 * 0x100)()
    rc_ptr_slot = ctypes.c_uint64.from_address(h_fc64 + 0x2dd7e08)
    orig_rc_val = rc_ptr_slot.value
    rc_ptr_slot.value = ctypes.addressof(rc_buf)

    dm_buf = (ctypes.c_uint8 * 0x100)()
    dm_ptr_slot = ctypes.c_uint64.from_address(h_fc64 + 0x2dd84c8)
    orig_dm_val = dm_ptr_slot.value
    dm_ptr_slot.value = ctypes.addressof(dm_buf)

    fn_native_fov = ctypes.CFUNCTYPE(ctypes.c_float, ctypes.c_float, ctypes.c_float)(h_fc64 + 0x234340)
    in_fov_deg = 90.0
    in_fov_rad = math.radians(in_fov_deg)
    base_16_9 = 16.0 / 9.0

    print("\n--- Aspect Ratio Matrix Verification (Zero-.text-Edit V2) ---")
    print(f"{'Ratio':<18} {'WxH':<11} {'FOVMode':<7} {'OutFOV(deg)':<12} {'Rect':<18} {'UiScale(M1-4)':<14}")
    try:
        for label, w, h in TEST_ASPECT_RATIOS:
            rects = (ctypes.c_int32 * 4)(100, 50, 900, 600)
            vp = MockDisplayViewportInfo()
            vp.bMultiMonSplit = 1
            vp.width = w
            vp.height = h
            vp.aspect = 1.7777778
            vp.rectsPtr = ctypes.addressof(rects)
            vp.rectCount = 3

            ow, oh, oa = ctypes.c_uint32(), ctypes.c_uint32(), ctypes.c_float()
            assert fn_norm(ctypes.byref(vp), ctypes.byref(ow), ctypes.byref(oh), ctypes.byref(oa))
            assert vp.bMultiMonSplit == 0
            assert vp.rectCount == 1
            assert list(rects) == [0, 0, w - 1, h - 1]
            assert abs(vp.aspect - (w / h)) < 1e-5

            aspect = w / h
            use_ws_fov = 1 if aspect > base_16_9 + 1e-4 else 0
            struct.pack_into("<iii", rc_buf, 0x48, 0, use_ws_fov, 0)
            out_fov_deg = math.degrees(fn_native_fov(in_fov_rad, aspect))
            if use_ws_fov == 0:
                assert abs(out_fov_deg - in_fov_deg) < 1e-3, f"{label}: Vert+ mismatch {out_fov_deg}"
            else:
                exp_deg = math.degrees(2.0 * math.atan(math.tan(0.5 * in_fov_rad) * (aspect / base_16_9)))
                assert abs(out_fov_deg - exp_deg) < 0.05, f"{label}: Hor+ mismatch {out_fov_deg} vs {exp_deg}"

            exp_uniform = min(w / 1280.0, h / 720.0)
            for mode in (0, 1, 2, 3, 4):
                io = UiLayoutIO()
                io.vpW = float(w)
                io.vpH = float(h)
                io.safeL = 0.0
                io.safeT = 0.0
                io.safeR = float(w)
                io.safeB = float(h)
                io.mode = mode
                fn_ui(ctypes.byref(io))
                assert abs(io.safeL) < 1e-9 and abs(io.safeT) < 1e-9
                assert abs(io.safeR - w) < 1e-9 and abs(io.safeB - h) < 1e-9
                if mode == 0:
                    assert abs(io.scaleX - w / 1280.0) < 1e-9
                    assert abs(io.scaleY - h / 720.0) < 1e-9
                else:
                    assert abs(io.scaleX - io.scaleY) < 1e-12
                    assert abs(io.scaleX - exp_uniform) < 1e-9
                    assert io.scaleX * 1280.0 <= w + 1e-6
                    assert io.scaleY * 720.0 <= h + 1e-6

            print(
                f"{label:<18} {f'{w}x{h}':<11} {'Hor+' if use_ws_fov else 'Vert+':<7} "
                f"{out_fov_deg:<12.2f} {f'[0,0,{w-1},{h-1}]':<18} {exp_uniform:<14.4f}"
            )
    finally:
        rc_ptr_slot.value = orig_rc_val
        dm_ptr_slot.value = orig_dm_val

    print("\n[ALL CHECKS PASSED] Zero .text bytes modified; all 18 aspect ratios verified live.")


if __name__ == "__main__":
    main()
