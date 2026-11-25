"""Memory-budget models for streaming one Skyrim cell onto the 3DS.

The hard numbers this model must respect (``docs/MEMORY_BUDGET.md``):

* VRAM is 6 MiB and is the only memory PICA can sample.  The sibling FNV port
  budgets **4 MiB of textures** after framebuffers/reserve.
* Textures must therefore be ETC1 (4 bpp) or ETC1A4 (8 bpp); BC formats cannot
  be sampled directly.
* Geometry lives in the linear heap, configured at 8-20 MiB.
* Storage is the SD card, so every miss is a file read; the model counts
  misses, not just resident bytes.

The model takes decoded sizes (not archive-compressed sizes) and simulates a
bounded LRU cache over the request stream produced by one cell's references.
A cell fits when the *unique* working set fits; the cache simulation answers
what happens when it does not.
"""

from __future__ import annotations

from collections import OrderedDict
from dataclasses import dataclass, field
from typing import Dict, Iterable, List, Optional, Sequence, Tuple

BYTES_PER_VERTEX = 24       # FNV port figure used by docs/MEMORY_BUDGET.md
BYTES_PER_INDEX = 2         # 16-bit triangle indices (3 per triangle)
VRAM_TOTAL = 6 * 1024 * 1024
TEXTURE_BUDGET = 4 * 1024 * 1024
GEOMETRY_BUDGETS = (8, 12, 16, 20)  # MiB, docs/MEMORY_BUDGET.md


@dataclass
class MeshStats:
    path: str
    nif_bytes: int = 0
    vertices: int = 0
    triangles: int = 0
    base_vertices: int = 0
    base_triangles: int = 0
    lod_vertices: int = 0
    lod_triangles: int = 0
    texture_paths: List[str] = field(default_factory=list)
    error: Optional[str] = None
    source: str = ""

    @property
    def geometry_bytes(self) -> int:
        return self.vertices * BYTES_PER_VERTEX + self.triangles * 3 * BYTES_PER_INDEX

    @property
    def base_geometry_bytes(self) -> int:
        return (
            self.base_vertices * BYTES_PER_VERTEX
            + self.base_triangles * 3 * BYTES_PER_INDEX
        )

    @property
    def lod_geometry_bytes(self) -> int:
        return self.geometry_bytes - self.base_geometry_bytes

    def as_dict(self) -> dict:
        return {
            "path": self.path,
            "nif_bytes": self.nif_bytes,
            "vertices": self.vertices,
            "triangles": self.triangles,
            "base_vertices": self.base_vertices,
            "base_triangles": self.base_triangles,
            "lod_vertices": self.lod_vertices,
            "lod_triangles": self.lod_triangles,
            "geometry_bytes": self.geometry_bytes,
            "base_geometry_bytes": self.base_geometry_bytes,
            "lod_geometry_bytes": self.lod_geometry_bytes,
            "textures": len(self.texture_paths),
            "error": self.error,
        }


@dataclass
class TextureStats:
    path: str
    width: int = 0
    height: int = 0
    format: str = ""
    mips: int = 0
    bc_bytes: int = 0
    etc1_bytes: int = 0
    etc1a4_bytes: int = 0
    error: Optional[str] = None
    source: str = ""

    @property
    def needs_alpha(self) -> bool:
        return self.format in ("DXT3", "DXT5", "ATI2/BC5", "R8G8B8A8_UNORM", "B8G8R8A8_UNORM")

    def as_dict(self) -> dict:
        return {
            "path": self.path,
            "width": self.width,
            "height": self.height,
            "format": self.format,
            "mips": self.mips,
            "bc_bytes": self.bc_bytes,
            "etc1_bytes": self.etc1_bytes,
            "etc1a4_bytes": self.etc1a4_bytes,
            "needs_alpha": self.needs_alpha,
            "error": self.error,
        }


@dataclass
class LruStats:
    capacity: int
    requests: int
    hits: int
    misses: int
    bytes_loaded: int
    evictions: int
    peak_bytes: int
    oversized: int
    final_entries: int

    @property
    def hit_ratio(self) -> float:
        return self.hits / self.requests if self.requests else 0.0

    def as_dict(self) -> dict:
        return {
            "capacity": self.capacity,
            "requests": self.requests,
            "hits": self.hits,
            "misses": self.misses,
            "hit_ratio": round(self.hit_ratio, 4),
            "bytes_loaded": self.bytes_loaded,
            "evictions": self.evictions,
            "peak_bytes": self.peak_bytes,
            "oversized": self.oversized,
            "final_entries": self.final_entries,
        }


def simulate_lru(requests: Sequence[Tuple[str, int]], capacity: int) -> LruStats:
    """Simulate an LRU cache of a fixed byte capacity.

    ``requests`` is a sequence of ``(key, size)`` pairs.  An item larger than
    the whole cache is counted as oversized and is not cached (it is still a
    miss and its bytes are counted as loaded).
    """
    cache: "OrderedDict[str, int]" = OrderedDict()
    used = 0
    hits = misses = loaded = evictions = oversized = 0
    peak = 0
    for key, size in requests:
        if key in cache:
            hits += 1
            cache.move_to_end(key)
            continue
        misses += 1
        loaded += size
        if size > capacity:
            oversized += 1
            continue
        while used + size > capacity and cache:
            _, old_size = cache.popitem(last=False)
            used -= old_size
            evictions += 1
        cache[key] = size
        used += size
        peak = max(peak, used)
    return LruStats(
        capacity=capacity,
        requests=len(requests),
        hits=hits,
        misses=misses,
        bytes_loaded=loaded,
        evictions=evictions,
        peak_bytes=peak,
        oversized=oversized,
        final_entries=len(cache),
    )


@dataclass
class CellWorkingSet:
    cell: dict
    ref_count: int = 0
    mesh_stats: List[MeshStats] = field(default_factory=list)
    texture_stats: List[TextureStats] = field(default_factory=list)
    missing_meshes: List[str] = field(default_factory=list)
    missing_textures: List[str] = field(default_factory=list)

    # -- aggregates -------------------------------------------------------

    @property
    def geometry_bytes(self) -> int:
        return sum(m.geometry_bytes for m in self.mesh_stats)

    @property
    def base_geometry_bytes(self) -> int:
        return sum(m.base_geometry_bytes for m in self.mesh_stats)

    @property
    def lod_geometry_bytes(self) -> int:
        return sum(m.lod_geometry_bytes for m in self.mesh_stats)

    @property
    def mesh_payload_bytes(self) -> int:
        return sum(m.nif_bytes for m in self.mesh_stats)

    @property
    def texture_bytes(self) -> int:
        return sum(t.etc1_bytes for t in self.texture_stats)

    @property
    def texture_alpha_bytes(self) -> int:
        return sum(t.etc1a4_bytes if t.needs_alpha else t.etc1_bytes for t in self.texture_stats)

    @property
    def bc_bytes(self) -> int:
        return sum(t.bc_bytes for t in self.texture_stats)

    @property
    def vertices(self) -> int:
        return sum(m.vertices for m in self.mesh_stats)

    @property
    def triangles(self) -> int:
        return sum(m.triangles for m in self.mesh_stats)

    def request_stream(self) -> List[Tuple[str, int]]:
        """One mesh request per reference, followed by its textures."""
        by_path = {m.path.lower(): m for m in self.mesh_stats}
        textures = {t.path.lower(): t for t in self.texture_stats}
        requests: List[Tuple[str, int]] = []
        # Preserve repetition across references by expanding the unique meshes
        # in the recorded per-reference order.  The caller stores that order in
        # ``cell['mesh_sequence']`` when available.
        sequence = self.cell.get("mesh_sequence") or [m.path for m in self.mesh_stats]
        for path in sequence:
            mesh = by_path.get(path.lower())
            if not mesh:
                continue
            requests.append(("mesh:" + mesh.path.lower(), mesh.geometry_bytes or 1))
            for texture_path in mesh.texture_paths:
                tex = textures.get(texture_path.lower())
                if tex:
                    requests.append(("tex:" + tex.path.lower(), tex.etc1_bytes or 1))
        return requests

    def assess(self, geometry_budgets: Sequence[int] = GEOMETRY_BUDGETS) -> dict:
        geometry_results = {
            "%dMiB" % mib: simulate_lru(
                [("mesh:" + m.path.lower(), m.geometry_bytes or 1) for m in self.mesh_stats],
                mib * 1024 * 1024,
            ).as_dict()
            for mib in geometry_budgets
        }
        texture_result = simulate_lru(
            [("tex:" + t.path.lower(), t.etc1_bytes or 1) for t in self.texture_stats],
            TEXTURE_BUDGET,
        ).as_dict()
        stream = self.request_stream()
        return {
            "cell": self.cell,
            "refs": self.ref_count,
            "unique_meshes": len(self.mesh_stats),
            "unique_textures": len(self.texture_stats),
            "vertices": self.vertices,
            "triangles": self.triangles,
            "geometry_bytes": self.geometry_bytes,
            "geometry_MiB": round(self.geometry_bytes / (1024 * 1024), 3),
            "base_geometry_bytes": self.base_geometry_bytes,
            "base_geometry_MiB": round(self.base_geometry_bytes / (1024 * 1024), 3),
            "lod_geometry_bytes": self.lod_geometry_bytes,
            "lod_geometry_MiB": round(self.lod_geometry_bytes / (1024 * 1024), 3),
            "geometry_fits": {
                "%dMiB" % mib: self.geometry_bytes <= mib * 1024 * 1024
                for mib in geometry_budgets
            },
            "base_geometry_fits": {
                "%dMiB" % mib: self.base_geometry_bytes <= mib * 1024 * 1024
                for mib in geometry_budgets
            },
            "mesh_payload_bytes": self.mesh_payload_bytes,
            "texture_bc_bytes": self.bc_bytes,
            "texture_etc1_bytes": self.texture_bytes,
            "texture_etc1_MiB": round(self.texture_bytes / (1024 * 1024), 3),
            "texture_etc1a4_bytes": self.texture_alpha_bytes,
            "texture_fits_4MiB": self.texture_bytes <= TEXTURE_BUDGET,
            "texture_fits_4MiB_with_alpha": self.texture_alpha_bytes <= TEXTURE_BUDGET,
            "vram_budget": TEXTURE_BUDGET,
            "geometry_unique_lru": geometry_results,
            "texture_unique_lru": texture_result,
            "request_stream_lru": {
                "texture": simulate_lru(
                    [r for r in stream if r[0].startswith("tex:")], TEXTURE_BUDGET
                ).as_dict(),
                "geometry_16MiB": simulate_lru(
                    [r for r in stream if r[0].startswith("mesh:")], 16 * 1024 * 1024
                ).as_dict(),
            },
            "missing_meshes": self.missing_meshes,
            "missing_textures": self.missing_textures,
        }


def measure_archived_cell(
    cell_files,
    mesh_archive,
    texture_archive,
    *,
    mesh_reader=None,
    texture_reader=None,
) -> CellWorkingSet:
    """Measure one derived cell against open BSA archives.

    ``cell_files`` is a :class:`skyrim.esm.CellFiles`.  ``mesh_reader`` and
    ``texture_reader`` default to the archive readers and exist so tests can
    inject synthetic data.
    """
    from . import dds as dds_mod
    from . import nif as nif_mod

    ws = CellWorkingSet(cell=cell_files.summary.as_dict(), ref_count=len(cell_files.refs))
    textures: Dict[str, TextureStats] = {}
    sequence: List[str] = []

    for mesh_path in cell_files.mesh_paths:
        sequence.append(mesh_path)
        entry = mesh_archive.find(mesh_path)
        if entry is None:
            ws.missing_meshes.append(mesh_path)
            ws.mesh_stats.append(MeshStats(path=mesh_path, error="not in mesh archive"))
            continue
        try:
            data = mesh_reader(mesh_path) if mesh_reader else mesh_archive.read_file(mesh_path)
            parsed = nif_mod.parse_nif(data)
            geo = parsed.geometry_stats()
            mesh = MeshStats(
                path=mesh_path,
                nif_bytes=len(data),
                vertices=geo["totals"]["vertices"],
                triangles=geo["totals"]["triangles"],
                base_vertices=geo["base_totals"]["vertices"],
                base_triangles=geo["base_totals"]["triangles"],
                lod_vertices=geo["lod_totals"]["vertices"],
                lod_triangles=geo["lod_totals"]["triangles"],
                texture_paths=parsed.texture_paths(),
                source=entry.path,
            )
        except Exception as exc:  # noqa: BLE001 - report, do not hide
            mesh = MeshStats(path=mesh_path, error="%s: %s" % (type(exc).__name__, exc))
        ws.mesh_stats.append(mesh)
        for texture_path in mesh.texture_paths:
            key = texture_path.lower()
            if key in textures:
                continue
            tex_entry = texture_archive.find(texture_path)
            if tex_entry is None:
                textures[key] = TextureStats(path=texture_path, error="not in texture archive")
                ws.missing_textures.append(texture_path)
                continue
            try:
                prefix = (
                    texture_reader(texture_path)
                    if texture_reader
                    else texture_archive.read_prefix(texture_path, 256)
                )
                info = dds_mod.parse_dds(prefix, require_payload=False)
                tex = TextureStats(
                    path=texture_path,
                    width=info.width,
                    height=info.height,
                    format=info.format_name,
                    mips=info.mip_count,
                    bc_bytes=dds_mod.bc_mipped_size(
                        info.width, info.height, info.kind, info.mip_count
                    )
                    if info.is_block_compressed
                    else info.all_mips_size,
                    etc1_bytes=dds_mod.etc1_mipped_size(info.width, info.height, info.mip_count),
                    etc1a4_bytes=dds_mod.etc1a4_mipped_size(
                        info.width, info.height, info.mip_count
                    ),
                    source=tex_entry.path,
                )
            except Exception as exc:  # noqa: BLE001
                tex = TextureStats(
                    path=texture_path, error="%s: %s" % (type(exc).__name__, exc)
                )
            textures[key] = tex

    ws.texture_stats = sorted(textures.values(), key=lambda t: t.path.lower())
    # Per-reference order for the cache simulation.
    by_mesh = {m.path.lower(): m for m in ws.mesh_stats}
    seq = []
    for ref in cell_files.refs:
        if ref.mesh:
            seq.append(ref.mesh)
    ws.cell["mesh_sequence"] = seq
    return ws


def vram_surface_count(width: int, height: int, budget: int = TEXTURE_BUDGET, mips: int = 1) -> int:
    """How many surfaces of this shape fit a VRAM budget (ETC1, all mips)."""
    per = ((width + 3) // 4) * ((height + 3) // 4) * 8
    if mips > 1:
        per = 0
        for level in range(mips):
            w = max(1, width >> level)
            h = max(1, height >> level)
            per += ((w + 3) // 4) * ((h + 3) // 4) * 8
    return budget // per if per else 0


def vram_summary() -> dict:
    top = 512 * 512 * 4 // 8  # 128 KiB per 512x512 ETC1 surface
    mips = 0
    for level in range(10):
        w = max(1, 512 >> level)
        h = max(1, 512 >> level)
        mips += ((w + 3) // 4) * ((h + 3) // 4) * 8
    return {
        "vram_total_bytes": VRAM_TOTAL,
        "texture_budget_bytes": TEXTURE_BUDGET,
        "etc1_512x512_top_bytes": top,
        "etc1_512x512_mips_bytes": mips,
        "surfaces_512x512_top": TEXTURE_BUDGET // top,
        "surfaces_512x512_mips": TEXTURE_BUDGET // mips,
        "etc1_bytes_per_mib": 1024 * 1024,
        "etc1_megapixels_4MiB": TEXTURE_BUDGET * 2 / 1_000_000,  # 4 bpp = 0.5 B/px
    }
