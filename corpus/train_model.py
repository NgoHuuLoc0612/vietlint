#!/usr/bin/env python3
"""
VietLint model training script v2.
GradientBoosting + cross-validation + correct ONNX input name.

Usage:
    python train_model.py --corpus corpus/corpus_raw.jsonl --output model.onnx
"""
import argparse
import json
import sys
from pathlib import Path
from collections import Counter

import numpy as np

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--corpus", required=True)
    parser.add_argument("--output", default="vietlint_classifier.onnx")
    parser.add_argument("--test-split", type=float, default=0.15)
    parser.add_argument("--min-confidence", type=float, default=0.70)
    args = parser.parse_args()

    # Load corpus
    examples = []
    with open(args.corpus, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                ex = json.loads(line)
                if ex.get("label", -1) >= 0 and ex.get("confidence", 0) >= args.min_confidence:
                    examples.append(ex)
            except json.JSONDecodeError:
                continue

    if not examples:
        print("No labeled examples found", file=sys.stderr)
        sys.exit(1)

    print(f"Loaded {len(examples)} examples")
    label_counts = Counter(ex["label"] for ex in examples)
    label_names = ["pure_ascii","pure_viet","mixed_viet","transliterated","abbreviation","unknown"]
    for lbl, cnt in sorted(label_counts.items()):
        name = label_names[lbl] if lbl < len(label_names) else str(lbl)
        print(f"  label {lbl} ({name}): {cnt}")

    # Extract features
    try:
        import sys as _sys
        _sys.path.insert(0, str(Path(__file__).parent.parent / "build" / "python"))
        import vietlint_core as core
        clf_engine = core.IdentifierClassifier()
        identifiers = [ex["id"] for ex in examples]
        X = np.array(clf_engine.extract_features(identifiers), dtype=np.float32)
    except ImportError as e:
        print(f"vietlint_core not available: {e}", file=sys.stderr)
        sys.exit(1)

    y = np.array([ex["label"] for ex in examples], dtype=np.int32)
    weights = np.array([ex.get("confidence", 1.0) for ex in examples], dtype=np.float32)

    # Filter out unknown label (5)
    mask = y < 5
    X, y, weights = X[mask], y[mask], weights[mask]
    print(f"After filtering unknowns: {len(y)} examples")

    # Train/test split with stratification
    from sklearn.model_selection import train_test_split, cross_val_score
    X_train, X_test, y_train, y_test, w_train, _ = train_test_split(
        X, y, weights, test_size=args.test_split, random_state=42,
        stratify=y if len(set(y)) > 1 else None
    )

    # GradientBoosting pipeline
    from sklearn.ensemble import GradientBoostingClassifier
    from sklearn.preprocessing import StandardScaler
    from sklearn.pipeline import Pipeline
    from sklearn.metrics import classification_report

    model = Pipeline([
        ("scaler", StandardScaler()),
        ("clf", GradientBoostingClassifier(
            n_estimators=300,
            max_depth=4,
            learning_rate=0.08,
            subsample=0.8,
            random_state=42,
            n_iter_no_change=15,
            validation_fraction=0.1,
        )),
    ])

    model.fit(X_train, y_train, clf__sample_weight=w_train)

    # Evaluation
    y_pred = model.predict(X_test)
    active_labels = sorted(set(y_test))
    active_names = [label_names[i] for i in active_labels if i < len(label_names)]
    print("\n" + classification_report(y_test, y_pred,
        labels=active_labels, target_names=active_names))

    # Cross-validation
    cv_scores = cross_val_score(model, X, y, cv=5, scoring="f1_macro")
    print(f"5-fold CV F1-macro: {cv_scores.mean():.3f} ± {cv_scores.std():.3f}")

    # Export to ONNX — IMPORTANT: input name must match C++ INPUT_NAME = "float_input"
    from skl2onnx import convert_sklearn
    from skl2onnx.common.data_types import FloatTensorType

    initial_type = [("float_input", FloatTensorType([None, X_train.shape[1]]))]
    onnx_model = convert_sklearn(
        model,
        initial_types=initial_type,
        options={id(model.named_steps["clf"]): {"zipmap": False}},
    )

    with open(args.output, "wb") as f:
        f.write(onnx_model.SerializeToString())
    print(f"\nModel exported to {args.output}")
    print(f"Input name: float_input (matches C++ INPUT_NAME)")
    print(f"Feature dim: {X_train.shape[1]}")

if __name__ == "__main__":
    main()
