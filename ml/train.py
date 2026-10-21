import glob, os
import numpy as np
import pandas as pd
from sklearn.tree import DecisionTreeClassifier, export_text
from sklearn.dummy import DummyClassifier
from sklearn.metrics import accuracy_score, confusion_matrix, classification_report
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

LABELS = ["resting", "carried", "drinking", "shaken"]
WIN = 100                  # 2 s at 50 Hz
TEST_SESSIONS = {3, 5}     # held-out recordings (no leakage)

NAMES = [f"{a}_{s}" for a in ["ax", "ay", "az", "gx", "gy", "gz"]
         for s in ["mean", "std", "min", "max"]] + ["mag_mean", "mag_std", "ps_max", "als_mean"]

def features(w):
    acc = w[:, 0:3] / 8192.0          # g
    gyr = w[:, 3:6] / 65.5            # dps
    mag = np.linalg.norm(acc, axis=1)
    f = []
    for ch in np.hstack([acc, gyr]).T:
        f += [ch.mean(), ch.std(), ch.min(), ch.max()]
    f += [mag.mean(), mag.std(), w[:, 6].max(), w[:, 7].mean()]
    return f

X_tr, y_tr, X_te, y_te = [], [], [], []
for path in sorted(glob.glob("data/*.csv")):
    label, sess = os.path.basename(path)[:-4].split("_")
    d = pd.read_csv(path)[["ax", "ay", "az", "gx", "gy", "gz", "ps", "als"]].to_numpy(dtype=float)
    for s in range(0, len(d) - WIN + 1, WIN):
        f = features(d[s:s + WIN])
        if int(sess) in TEST_SESSIONS:
            X_te.append(f); y_te.append(label)
        else:
            X_tr.append(f); y_tr.append(label)

X_tr, X_te = np.array(X_tr), np.array(X_te)
print("train windows per class:", {l: y_tr.count(l) for l in LABELS})
print("test  windows per class:", {l: y_te.count(l) for l in LABELS})

base = DummyClassifier(strategy="most_frequent").fit(X_tr, y_tr)
print(f"\nBaseline (most frequent) accuracy: {accuracy_score(y_te, base.predict(X_te)):.3f}")

model = DecisionTreeClassifier(max_depth=5, random_state=0).fit(X_tr, y_tr)
pred = model.predict(X_te)
print(f"Decision tree accuracy:            {accuracy_score(y_te, pred):.3f}\n")
print(classification_report(y_te, pred, labels=LABELS, zero_division=0))

cm = confusion_matrix(y_te, pred, labels=LABELS)
print("Confusion matrix (rows = true, cols = predicted):")
print(cm)

fig, ax = plt.subplots(figsize=(5, 4))
ax.imshow(cm, cmap="Blues")
ax.set_xticks(range(4)); ax.set_xticklabels(LABELS, rotation=30)
ax.set_yticks(range(4)); ax.set_yticklabels(LABELS)
for i in range(4):
    for j in range(4):
        ax.text(j, i, cm[i, j], ha="center", va="center")
ax.set_xlabel("predicted"); ax.set_ylabel("true")
plt.tight_layout()
plt.savefig("ml/confusion_matrix.png", dpi=150)

print("\nTop features:")
for i in np.argsort(model.feature_importances_)[::-1][:6]:
    print(f"  {NAMES[i]}: {model.feature_importances_[i]:.3f}")
print("\n" + export_text(model, feature_names=NAMES))