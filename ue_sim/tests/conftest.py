import pathlib
import sys

# Let `pytest ue_sim/tests` work from the repo root without `pip install -e ue_sim`.
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
