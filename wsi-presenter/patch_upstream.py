from pathlib import Path
import sys

path = Path(sys.argv[1] if len(sys.argv) > 1 else "lens_presenter.py")
s = path.read_text(encoding="utf-8")
s = s.replace("from windows_capture import WindowsCapture", "import dxcam")
s = s.replace("A frame arrives from Windows Graphics Capture,", "A frame arrives from DXGI Desktop Duplication,")
s = s.replace("| 0x00000080 | 0x08000000)", "| 0x00000080 | 0x08000000 | 0x00000020)")

start = s.index("    # ---- capture: one slot")
end = s.index("    # ---- commands on stdin")
replacement = '''    # ---- DXGI Desktop Duplication: one slot, newest frame wins
    slot = {"frame": np.empty((H, W, 4), np.uint8), "spare": np.empty((H, W, 4), np.uint8),
            "ts": 0.0, "new": False, "dropped": 0, "arrived": 0,
            "last": time.perf_counter(), "unfit": 0, "size": (0, 0), "closed": False}
    crop = {"x": args.crop[0], "y": args.crop[1]}
    cv = threading.Condition()
    kind, _, ref = args.source.partition(":")
    cap = None
    capture_stop = threading.Event()

    class CaptureControl:
        def stop(self):
            capture_stop.set()
            try:
                if cap is not None:
                    cap.stop()
            except Exception:
                pass

    ctl = None
    if kind == "monitor":
        output_idx = int(ref)
        cap = dxcam.create(output_idx=output_idx, output_color="BGRA")
        region = (crop["x"], crop["y"], crop["x"] + W, crop["y"] + H)

        def capture_loop():
            try:
                cap.start(region=region, target_fps=240, video_mode=True)
                while not capture_stop.is_set():
                    frame = cap.get_latest_frame()
                    if frame is None:
                        time.sleep(0.001)
                        continue
                    fh, fw = frame.shape[:2]
                    if fh < H or fw < W:
                        slot["unfit"] += 1
                        slot["size"] = (fw, fh)
                        continue
                    with cv:
                        np.copyto(slot["spare"], frame[:H, :W, :])
                        slot["frame"], slot["spare"] = slot["spare"], slot["frame"]
                        slot["ts"] = time.perf_counter()
                        if slot["new"]:
                            slot["dropped"] += 1
                        slot["new"] = True
                        slot["arrived"] += 1
                        slot["last"] = time.perf_counter()
                        cv.notify()
            except Exception as exc:
                say("DXGI capture failed: %s" % exc)
            finally:
                slot["closed"] = True
                try:
                    cap.stop()
                except Exception:
                    pass

        ctl = CaptureControl()
        threading.Thread(target=capture_loop, name="dxgi-capture", daemon=True).start()
    elif kind == "pattern":
        rng = np.random.default_rng(1)
        still = rng.integers(0, 255, (H, W, 4), np.uint8)
        still[::7, :, :] = 230
        still[:, :, 3] = 255
        with cv:
            np.copyto(slot["frame"], still)
            slot["ts"] = time.perf_counter()
            slot["new"] = True
            slot["arrived"] += 1
    else:
        sys.exit("DXGI build accepts monitor:<index> or pattern")

'''
s = s[:start] + replacement + s[end:]
path.write_text(s, encoding="utf-8")
