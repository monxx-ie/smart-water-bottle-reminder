"""Rule-based baseline vs. the decision tree, on the same held-out sessions.

Run from the project root:  python ml/baseline.py
Writes ml/results.txt (per-class precision/recall for both models).
The rule thresholds are hand-set from the TRAINING sessions only (1, 2, 4, 6): resting <= 0.11 dps, drinking <= 73.7, shaken >= 116.9 -> cut at 95.
"""
import glob, os
import numpy as np
import pandas as pd
from sklearn.tree import DecisionTreeClassifier
from sklearn.dummy import DummyClassifier
from sklearn.metrics import accuracy_score, classification_report, confusion_matrix

LABELS = ["resting", "carried", "drinking", "shaken"]
WIN = 100
TEST_SESSIONS = {3, 5}

def features(w):
    acc = w[:, 0:3] / 8192.0
    gyr = w[:, 3:6] / 65.5
    mag = np.linalg.norm(acc, axis=1)
    f = []
    for ch in np.hstack([acc, gyr]).T:
        f += [ch.mean(), ch.std(), ch.min(), ch.max()]
    f += [mag.mean(), mag.std(), w[:, 6].max(), w[:, 7].mean()]
    return f

# indices into the 28-feature vector
GYR_STD = [3 * 4 + 1, 4 * 4 + 1, 5 * 4 + 1]   # gx_std, gy_std, gz_std
AZ_MEAN = 2 * 4 + 0
PS_MAX = 26

def rule_predict(f):
    """Simple threshold rules (no learning)."""
    g = np.mean([f[i] for i in GYR_STD])      # average gyro std, dps
    if g < 1.0:
        return "resting"                       # almost no rotation
    if g > 95.0:
        return "shaken"                        # violent rotation
    if f[PS_MAX] > 60 or f[AZ_MEAN] < 0.7:
        return "drinking"                      # object near mouth / bottle tilted
    return "carried"

X_tr, y_tr, X_te, y_te = [], [], [], []
for path in sorted(glob.glob("data/*_0?.csv")):
    label, sess = os.path.basename(path)[:-4].split("_")
    d = pd.read_csv(path)[["ax", "ay", "az", "gx", "gy", "gz", "ps", "als"]].to_numpy(dtype=float)
    for s in range(0, len(d) - WIN + 1, WIN):
        f = features(d[s:s + WIN])
        if int(sess) in TEST_SESSIONS:
            X_te.append(f); y_te.append(label)
        else:
            X_tr.append(f); y_tr.append(label)
X_tr, X_te = np.array(X_tr), np.array(X_te)

out = []
def show(name, pred):
    acc = accuracy_score(y_te, pred)
    out.append(f"=== {name}: accuracy {acc:.3f} ===")
    out.append(classification_report(y_te, pred, labels=LABELS, zero_division=0))
    out.append("confusion matrix (rows = true, cols = predicted; order: " + ", ".join(LABELS) + ")")
    out.append(str(confusion_matrix(y_te, pred, labels=LABELS)) + "\n")

show("Majority-class baseline", DummyClassifier(strategy="most_frequent").fit(X_tr, y_tr).predict(X_te))
show("Threshold-rule baseline", [rule_predict(f) for f in X_te])
tree = DecisionTreeClassifier(max_depth=5, random_state=0).fit(X_tr, y_tr)
show("Decision tree (deployed)", tree.predict(X_te))
out.append(f"train windows: {len(y_tr)}   test windows: {len(y_te)}")
text = "\n".join(out)
print(text)
open("ml/results.txt", "w").write(text + "\n")
