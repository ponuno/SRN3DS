"""Offline contracts for the specialised menu shader paths.

This package is host build tooling only.  It does not render, translate SM3 for
the guest, or modify runtime/core/COM/PICA code.  See ``docs/MENU_PREPROCESS.md``
for the input and output formats and the future loader hand-off.
"""

from .common import (
    BudgetError,
    ContractError,
    DataError,
    Limits,
    MemoryMeter,
    MenuPreprocessError,
)
from .signatures import SIGNATURES, Signature, normalize_signature

__all__ = [
    "BudgetError",
    "ContractError",
    "DataError",
    "Limits",
    "MemoryMeter",
    "MenuPreprocessError",
    "SIGNATURES",
    "Signature",
    "normalize_signature",
]
