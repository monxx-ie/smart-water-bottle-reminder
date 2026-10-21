import sys, time, csv, os
import serial

PORT = "COM3"
BAUD = 115200
STATES = ["RESTING", "CARRIED", "DRINKING", "SHAKEN"]
HOWTO = {
    "RESTING":  "board still on the table",
    "CARRIED":  "hold it and swing your arm back and forth gently",
    "DRINKING": "pick up, tilt to your mouth, tilt back, put down",
    "SHAKEN":   "shake it hard",
}
N = 20          # trials per state
TRIAL_S = 6     # length of each trial
GAP_S = 3       # get-ready time between trials
CSV = "data/trials.csv"


def connect():
    while True:
        try:
            return serial.Serial(PORT, BAUD, timeout=0.2)
        except (serial.SerialException, OSError):
            print("  (serial port lost, retrying...)", flush=True)
            time.sleep(1)


ser = None


def read_until(t_end):
    """returns (last predicted state, connection_was_lost)"""
    global ser
    last, lost = None, False
    while time.time() < t_end:
        try:
            line = ser.readline().decode("ascii", errors="ignore").strip()
        except (serial.SerialException, OSError):
            lost = True
            try:
                ser.close()
            except Exception:
                pass
            time.sleep(1)
            ser = connect()
            continue
        if "state=" in line:
            try:
                last = line.split("state=")[1].split()[0]
            except IndexError:
                pass
    return last, lost


def load():
    res = {s: [] for s in STATES}
    if os.path.exists(CSV):
        with open(CSV) as f:
            for r in csv.DictReader(f):
                if r["true"] in res:
                    res[r["true"]].append((r["true"], int(r["trial"]), r["predicted"], int(r["correct"])))
    return res


def save(res):
    os.makedirs("data", exist_ok=True)
    with open(CSV, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["true", "trial", "predicted", "correct"])
        for s in STATES:
            w.writerows(res[s])


def summary(res):
    flat = [r for s in STATES for r in res[s]]
    if not flat:
        print("no trial data yet")
        return
    ok = sum(r[3] for r in flat)
    print("\n=== RESULTS ===")
    print(f"overall: {ok}/{len(flat)} = {100 * ok / len(flat):.1f}%\n")
    print("true\\pred".ljust(11) + "".join(s.ljust(10) for s in STATES))
    for t in STATES:
        row = [sum(1 for r in res[t] if r[2] == p) for p in STATES]
        print(t.ljust(11) + "".join(str(c).ljust(10) for c in row)
              + f"({row[STATES.index(t)]}/{len(res[t])})")


args = [a.upper() for a in sys.argv[1:]]
res = load()
if args == ["SUMMARY"]:
    summary(res)
    sys.exit()
todo = args or STATES
for a in todo:
    if a not in STATES:
        print("unknown state:", a)
        sys.exit(1)

ser = connect()
for st in todo:
    input(f"\n=== {st}: {HOWTO[st]}\nPress Enter to start {N} trials...")
    res[st] = []
    k = 1
    while k <= N:
        print(f"  trial {k}/{N}: get ready...", end="", flush=True)
        read_until(time.time() + GAP_S)
        print(f" GO ({st})", end="", flush=True)
        pred, lost = read_until(time.time() + TRIAL_S)
        if lost:
            print("  -> connection hiccup, repeating this trial")
            continue
        ok = (pred == st)
        print(f"  -> {pred}  {'OK' if ok else 'WRONG'}")
        res[st].append((st, k, pred or "NONE", int(ok)))
        save(res)
        k += 1
ser.close()
summary(res)