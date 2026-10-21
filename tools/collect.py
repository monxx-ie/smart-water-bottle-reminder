import sys, os, time
import serial

PORT = "COM3"
BAUD = 115200
LABELS = ["resting", "carried", "drinking", "shaken"]

if len(sys.argv) != 3 or sys.argv[1] not in LABELS:
    print("usage: collect.py <resting|carried|drinking|shaken> <seconds>")
    sys.exit(1)

label, secs = sys.argv[1], float(sys.argv[2])
os.makedirs("data", exist_ok=True)
n = 1
while os.path.exists(f"data/{label}_{n:02d}.csv"):
    n += 1
path = f"data/{label}_{n:02d}.csv"

ser = serial.Serial(PORT, BAUD, timeout=1)
for i in (3, 2, 1):
    print(f"starting in {i}...", flush=True)
    time.sleep(1)
ser.reset_input_buffer()
ser.readline()                      # drop a possibly partial line

print(f"RECORDING {label} for {secs:.0f} s  ->  {path}", flush=True)
rows, errors = [], 0
t_end = time.time() + secs
while time.time() < t_end:
    line = ser.readline().decode("ascii", errors="ignore").strip()
    if line.startswith("D,"):
        p = line.split(",")
        if len(p) == 10:
            try:
                rows.append([int(x) for x in p[1:]])
            except ValueError:
                errors += 1
    elif line.startswith("E,"):
        errors += 1
ser.close()

with open(path, "w") as f:
    f.write("ms,ax,ay,az,gx,gy,gz,ps,als\n")
    for r in rows:
        f.write(",".join(map(str, r)) + "\n")

print(f"saved {len(rows)} samples, {errors} errors")
if len(rows) > 2:
    d = [rows[i + 1][0] - rows[i][0] for i in range(len(rows) - 1)]
    print(f"sample interval: mean {sum(d)/len(d):.1f} ms, min {min(d)} ms, max {max(d)} ms")