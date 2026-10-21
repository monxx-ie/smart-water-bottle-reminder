import re, sys, time, os
import serial

PORT = "COM3"
BAUD = 115200
MINUTES = float(sys.argv[1]) if len(sys.argv) > 1 else 15
os.makedirs("logs", exist_ok=True)
PATH = "logs/soak_run.txt"


def connect():
    while True:
        try:
            return serial.Serial(PORT, BAUD, timeout=1)
        except (serial.SerialException, OSError):
            time.sleep(1)


ser = connect()
t_start = time.time()
t_end = t_start + MINUTES * 60
state_ms = []
reboots = reminders = lost = 0
drops = errs = min_heap = None
min_hwm = {}
next_report = 60

print(f"logging {MINUTES:g} min to {PATH}  (Ctrl+C stops early)")
with open(PATH, "w") as log:
    try:
        while time.time() < t_end:
            try:
                line = ser.readline().decode("ascii", errors="ignore").strip()
            except (serial.SerialException, OSError):
                lost += 1
                try:
                    ser.close()
                except Exception:
                    pass
                ser = connect()
                continue
            if not line:
                continue
            log.write(f"{time.time() - t_start:8.1f}s  {line}\n")
            log.flush()

            m = re.match(r"\[(\d+) ms\] state=(\w+)", line)
            if m:
                state_ms.append(int(m.group(1)))
            if "on-device inference starting" in line:
                reboots += 1
            if "REMINDER" in line:
                reminders += 1
            m = re.search(r"queue drops=(\d+) imu_errors=(\d+)", line)
            if m:
                drops, errs = int(m.group(1)), int(m.group(2))
            m = re.search(r"heap=(\d+) B HWM: (.*)", line)
            if m:
                h = int(m.group(1))
                min_heap = h if min_heap is None else min(min_heap, h)
                for name, val in re.findall(r"(\w+)=(\d+)", m.group(2)):
                    v = int(val)
                    min_hwm[name] = v if name not in min_hwm else min(min_hwm[name], v)

            if time.time() - t_start >= next_report:
                print(f"  {int(next_report / 60)} min: {len(state_ms)} predictions so far", flush=True)
                next_report += 60
    except KeyboardInterrupt:
        print("stopped early")

ser.close()
gaps = [b - a for a, b in zip(state_ms, state_ms[1:])]
print("\n=== SOAK TEST RESULT ===")
print(f"duration:             {(time.time() - t_start) / 60:.1f} min")
print(f"predictions:          {len(state_ms)}")
print(f"max gap:              {max(gaps) if gaps else 'n/a'} ms (should be about 1000)")
print(f"lowest free heap:     {min_heap} B")
print(f"lowest stack headroom (words): {min_hwm}")
print(f"queue drops:          {drops}")
print(f"IMU errors:           {errs}")
print(f"reboots:              {reboots}")
print(f"serial drops:         {lost}")
print(f"reminders fired:      {reminders}")