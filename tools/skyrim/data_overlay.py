"""Skyrim LE Data overlay: plugins.txt/loadorder.txt, archives, loose files.

The vanilla path (``Skyrim.esm`` + ``Update.esm`` and the shipped ``Skyrim -
*.bsa`` archives) keeps working with no plugin list present.  A list is only
adopted when it actually carries an LE active marker (``*``), so the Steam
``plugins.txt`` that merely lists downloaded DLC does not switch the port into
a broken load order.

Precedence, highest first
-------------------------
1. loose file under ``Data`` (any extension, including ``.esp``/``.bsa`` only
   for the loader itself -- they are not asset sources);
2. the last mounted BSA that contains the path (later plugin / Update wins);
3. an earlier BSA.

Mount order is the load order: ``skyrim.esm`` first, official archives in
their canonical order, then each active plugin's archives.  Mod archives are
``<plugin stem>.bsa`` and ``<plugin stem> - *.bsa`` (matched
case-insensitively, sorted), which covers the two-archive ``Textures`` split
used by most LE mods.

Errors are named and explicit: ``MissingMasterError``,
``MissingPluginError``, ``PluginFormatError``, ``PluginOrderError``,
``DataRootError``, ``ArchiveError``.  Nothing is silently skipped; a corrupt
archive raises instead of making an asset quietly disappear.
"""

from __future__ import annotations

import os
import struct
import zlib
from dataclasses import dataclass, field
from typing import Dict, Iterable, List, Mapping, Optional, Sequence, Tuple

from . import bsa

PLUGIN_EXTENSIONS = (".esm", ".esp", ".esl")
ARCHIVE_EXTENSION = ".bsa"

#: Shipped archives in the order TESV mounts them.  ``Update.bsa`` is last.
OFFICIAL_ARCHIVES = {
    "skyrim.esm": (
        "Skyrim - Animations.bsa",
        "Skyrim - Interface.bsa",
        "Skyrim - Meshes.bsa",
        "Skyrim - Misc.bsa",
        "Skyrim - Shaders.bsa",
        "Skyrim - Sounds.bsa",
        "Skyrim - Textures.bsa",
    ),
    "update.esm": ("Update.bsa",),
}

#: Plugin header flags (TES4 record flags on the TES4 record itself).
FLAG_MASTER = 0x00000001
FLAG_LIGHT_MASTER = 0x00000200  # SE/ESL; not valid in LE
FLAG_COMPRESSED = 0x00040000

HEADER_RECORD_SIZE = 24
MAX_HEADER_PAYLOAD = 8 * 1024 * 1024


class OverlayError(Exception):
    """Base class; ``code`` is stable for the CLI/tests."""

    code = "overlay-error"

    def __init__(self, message: str):
        super().__init__(message)
        self.message = message


class DataRootError(OverlayError):
    code = "data-root"


class PluginListError(OverlayError):
    code = "plugin-list"


class PluginFormatError(OverlayError):
    code = "plugin-format"


class PluginOrderError(OverlayError):
    code = "plugin-order"


class MissingPluginError(OverlayError):
    code = "missing-plugin"


class MissingMasterError(OverlayError):
    code = "missing-master"

    def __init__(self, master: str, plugin: str, reason: str = "not found in Data"):
        self.master = master
        self.plugin = plugin
        self.reason = reason
        super().__init__(
            "plugin '%s' requires master '%s', which is %s" % (plugin, master, reason)
        )


class ArchiveError(OverlayError):
    code = "archive"


# ---------------------------------------------------------------------------
# Paths and plugin text files
# ---------------------------------------------------------------------------


def normalize_virtual_path(path: str) -> str:
    """Backslashes, no leading separator, no empty components."""
    path = path.replace("/", "\\").strip()
    parts = [p for p in path.split("\\") if p not in ("", ".")]
    if any(p == ".." for p in parts):
        raise PluginListError("virtual path escapes Data: %r" % path)
    return "\\".join(parts)


def resolve_data_root(explicit: Optional[str] = None,
                      env: Optional[Mapping[str, str]] = None) -> str:
    """Resolve ``TESV_DATA_ROOT`` / explicit path / well-known defaults.

    An explicit or environment path that does not exist is an error; the
    defaults are only probes.
    """
    env = os.environ if env is None else env
    explicit_paths = []
    if explicit:
        explicit_paths.append(("argument", explicit))
    for key in ("TESV_DATA_ROOT", "SKYRIM_DATA"):
        if env.get(key):
            explicit_paths.append((key, env[key]))
    for origin, candidate in explicit_paths:
        full = os.path.abspath(os.path.expanduser(candidate))
        if not os.path.isdir(full):
            raise DataRootError("%s points at '%s', which is not a directory"
                                % (origin, candidate))
        if not os.path.isfile(os.path.join(full, "Skyrim.esm")):
            raise DataRootError("%s '%s' has no Skyrim.esm" % (origin, full))
        return full

    probes = [
        os.path.join(os.getcwd(), "Data"),
        r"F:\Skyrim\Data",
        r"C:\Program Files (x86)\Steam\steamapps\common\Skyrim\Data",
    ]
    for candidate in probes:
        if os.path.isfile(os.path.join(candidate, "Skyrim.esm")):
            return os.path.abspath(candidate)
    raise DataRootError(
        "no Data directory found; set TESV_DATA_ROOT or pass --data "
        "(searched: %s)" % ", ".join(probes)
    )


def _read_text(path: str) -> str:
    try:
        with open(path, "rb") as handle:
            raw = handle.read()
    except OSError as exc:
        raise PluginListError("cannot read '%s': %s" % (path, exc)) from exc
    if raw.startswith(b"\xef\xbb\xbf"):
        raw = raw[3:]
    try:
        return raw.decode("utf-8")
    except UnicodeDecodeError:
        return raw.decode("cp1252", "replace")


@dataclass
class PluginListEntry:
    name: str
    active: bool
    line: int

    def as_dict(self) -> dict:
        return {"name": self.name, "active": self.active, "line": self.line}


def parse_plugin_list_text(text: str, *, source: str = "<text>",
                           explicit_active: bool = True) -> List[PluginListEntry]:
    """Parse LE plugins.txt.

    ``*Name.esp`` is active.  A list with no ``*`` at all is treated as a plain
    order (all listed entries active) only when ``explicit_active`` is false;
    that is the mode used for an explicitly supplied ``plugins.txt``.
    """
    entries: List[PluginListEntry] = []
    seen = set()
    has_star = False
    if text.startswith("\ufeff"):
        text = text[1:]
    for number, raw_line in enumerate(text.splitlines(), 1):
        line = raw_line.strip()
        if not line or line.startswith("#") or line.startswith(";"):
            continue
        active = line.startswith("*")
        if active:
            has_star = True
            line = line[1:].strip()
        if not line:
            continue
        if "/" in line or "\\" in line:
            raise PluginListError("%s:%d: plugin names must not contain a path: %r"
                                  % (source, number, line))
        lower = line.lower()
        if not lower.endswith(PLUGIN_EXTENSIONS):
            raise PluginListError("%s:%d: not a plugin extension: %r"
                                  % (source, number, line))
        if lower in seen:
            raise PluginListError("%s:%d: duplicate plugin %r" % (source, number, line))
        seen.add(lower)
        entries.append(PluginListEntry(line, active, number))
    if not has_star and entries and explicit_active:
        for entry in entries:
            entry.active = True
    return entries


def parse_plugin_list(path: str, *, require_active: bool = False) -> List[PluginListEntry]:
    entries = parse_plugin_list_text(_read_text(path), source=path)
    if require_active and not any(e.active for e in entries):
        raise PluginListError("%s has no active (*) plugins" % path)
    return entries


def parse_load_order(path: str) -> List[str]:
    """``loadorder.txt``: bare load order, optional ``*`` and comments."""
    entries = parse_plugin_list_text(_read_text(path), source=path)
    return [entry.name for entry in entries]


# ---------------------------------------------------------------------------
# ESP/ESM header
# ---------------------------------------------------------------------------


@dataclass
class PluginHeader:
    name: str
    path: str
    size: int
    masters: List[str] = field(default_factory=list)
    is_master: bool = False
    is_light: bool = False
    form_version: int = 0
    hed_version: float = 0.0
    record_count: int = 0
    author: str = ""
    compression: str = "none"  # none | zlib

    def as_dict(self) -> dict:
        return {
            "name": self.name,
            "path": self.path,
            "size": self.size,
            "masters": list(self.masters),
            "is_master": self.is_master,
            "is_light": self.is_light,
            "form_version": self.form_version,
            "hed_version": self.hed_version,
            "record_count": self.record_count,
            "author": self.author,
        }


def _decode_subrecords(payload: bytes) -> Iterable[Tuple[bytes, bytes]]:
    pos = 0
    end = len(payload)
    while pos + 6 <= end:
        kind = payload[pos:pos + 4]
        size = struct.unpack_from("<H", payload, pos + 4)[0]
        pos += 6
        if pos + size > end:
            raise PluginFormatError("truncated %r subrecord" % kind)
        yield kind, payload[pos:pos + size]
        pos += size


def read_plugin_header(path: str) -> PluginHeader:
    name = os.path.basename(path)
    try:
        size = os.path.getsize(path)
        with open(path, "rb") as handle:
            head = handle.read(HEADER_RECORD_SIZE)
            if len(head) != HEADER_RECORD_SIZE:
                raise PluginFormatError("%s: shorter than a TES4 record header" % name)
            kind, data_size, flags, _form, _vc0, _vc1, form_version, _unknown = struct.unpack(
                "<4sIIIHHHH", head)
            if kind != b"TES4":
                raise PluginFormatError("%s: first record is %r, not TES4"
                                        % (name, kind))
            if data_size > MAX_HEADER_PAYLOAD:
                raise PluginFormatError("%s: TES4 payload %d exceeds %d"
                                        % (name, data_size, MAX_HEADER_PAYLOAD))
            payload = handle.read(data_size)
            if len(payload) != data_size:
                raise PluginFormatError("%s: truncated TES4 payload" % name)
            compression = "none"
            if flags & FLAG_COMPRESSED:
                compression = "zlib"
                if len(payload) < 4:
                    raise PluginFormatError("%s: compressed TES4 has no size" % name)
                unpacked = struct.unpack_from("<I", payload, 0)[0]
                if unpacked > MAX_HEADER_PAYLOAD:
                    raise PluginFormatError("%s: TES4 inflates to %d bytes"
                                            % (name, unpacked))
                try:
                    payload = zlib.decompress(payload[4:])
                except zlib.error as exc:
                    raise PluginFormatError("%s: TES4 deflate: %s" % (name, exc)) from exc
                if len(payload) != unpacked:
                    raise PluginFormatError("%s: TES4 inflated to %d, expected %d"
                                            % (name, len(payload), unpacked))
    except OSError as exc:
        raise PluginFormatError("cannot read '%s': %s" % (path, exc)) from exc

    header = PluginHeader(name=name, path=os.path.abspath(path), size=size,
                          is_master=bool(flags & FLAG_MASTER),
                          is_light=bool(flags & FLAG_LIGHT_MASTER),
                          form_version=form_version, compression=compression)
    if name.lower().endswith(".esm"):
        header.is_master = True
    if name.lower().endswith(".esl"):
        header.is_light = True

    for sub_kind, data in _decode_subrecords(payload):
        if sub_kind == b"HEDR" and len(data) >= 12:
            header.hed_version = struct.unpack_from("<f", data, 0)[0]
            header.record_count = struct.unpack_from("<i", data, 4)[0]
        elif sub_kind == b"CNAM":
            header.author = data.rstrip(b"\x00").decode("cp1252", "replace")
        elif sub_kind == b"MAST":
            master = data.rstrip(b"\x00").decode("cp1252", "replace").strip()
            if master:
                header.masters.append(master)
    return header


# ---------------------------------------------------------------------------
# Load order
# ---------------------------------------------------------------------------


@dataclass
class LoadedPlugin:
    header: PluginHeader
    active: bool
    declared_index: int

    @property
    def name(self) -> str:
        return self.header.name

    def as_dict(self) -> dict:
        out = self.header.as_dict()
        out["active"] = self.active
        return out


def scan_plugin_files(data_root: str) -> List[str]:
    """All plugin files under Data, official-first and then case-fold sorted."""
    try:
        names = os.listdir(data_root)
    except OSError as exc:
        raise DataRootError("cannot list '%s': %s" % (data_root, exc)) from exc
    plugins = [n for n in names if n.lower().endswith(PLUGIN_EXTENSIONS)
               and os.path.isfile(os.path.join(data_root, n))]

    def key(name: str) -> Tuple[int, str]:
        lower = name.lower()
        if lower == "skyrim.esm":
            return (0, lower)
        if lower == "update.esm":
            return (1, lower)
        return (2, lower)

    return sorted(plugins, key=key)


def discover_plugin_lists(data_root: str,
                          env: Optional[Mapping[str, str]] = None
                          ) -> Tuple[Optional[str], Optional[str], str]:
    """Return (plugins.txt, loadorder.txt, note).

    A discovered plugins.txt is adopted only when it has an active marker;
    otherwise the vanilla/default order is used and the note says why.
    """
    env = os.environ if env is None else env
    notes = []
    game_dir = os.path.dirname(os.path.abspath(data_root))
    candidates = [
        os.path.join(data_root, "plugins.txt"),
        os.path.join(game_dir, "plugins.txt"),
    ]
    local = env.get("LOCALAPPDATA")
    if local:
        candidates.append(os.path.join(local, "Skyrim", "plugins.txt"))
    plugins_path = None
    for candidate in candidates:
        if not os.path.isfile(candidate):
            continue
        text = _read_text(candidate)
        if any(line.lstrip().startswith("*") for line in text.splitlines()):
            plugins_path = candidate
            break
        notes.append("ignored %s: no active (*) entries" % candidate)
    loadorder_path = None
    for candidate in (
        os.path.join(data_root, "loadorder.txt"),
        os.path.join(game_dir, "loadorder.txt"),
    ):
        if os.path.isfile(candidate):
            loadorder_path = candidate
            break
    return plugins_path, loadorder_path, "; ".join(notes)


def build_load_order(data_root: str,
                     plugins_path: Optional[str] = None,
                     loadorder_path: Optional[str] = None,
                     auto_discover: bool = True) -> Tuple[List[LoadedPlugin], dict]:
    """Resolve the deterministic active load order.

    Returns ``(plugins, report)`` where ``plugins`` is topological: every
    master precedes its dependent.  Raises ``MissingMasterError`` for an
    absent or disabled master, ``MissingPluginError`` for an active plugin
    whose file is absent, and ``PluginOrderError`` for a cycle.
    """
    if plugins_path is not None and not os.path.isfile(plugins_path):
        raise PluginListError("plugins.txt not found: '%s'" % plugins_path)
    if loadorder_path is not None and not os.path.isfile(loadorder_path):
        raise PluginListError("loadorder.txt not found: '%s'" % loadorder_path)

    discovered_note = ""
    if plugins_path is None and loadorder_path is None and auto_discover:
        plugins_path, loadorder_path, discovered_note = discover_plugin_lists(data_root)

    available = {name.lower(): name for name in scan_plugin_files(data_root)}

    declared: List[str] = []
    active_names = set()
    if loadorder_path:
        declared = parse_load_order(loadorder_path)
    plugin_entries = parse_plugin_list(plugins_path) if plugins_path else []
    if plugin_entries:
        active_names = {entry.name.lower() for entry in plugin_entries if entry.active}
        if not declared:
            declared = [entry.name for entry in plugin_entries]
        else:
            known = {name.lower() for name in declared}
            for entry in plugin_entries:
                if entry.name.lower() not in known:
                    declared.append(entry.name)
                    known.add(entry.name.lower())
    if not declared:
        declared = scan_plugin_files(data_root)
    if not plugin_entries:
        # No plugin list: every declared plugin is active (vanilla path).
        active_names = {name.lower() for name in declared}

    headers: Dict[str, PluginHeader] = {}
    missing_plugins: List[str] = []
    ordered_names: List[str] = []
    for name in declared:
        key = name.lower()
        if key in {n.lower() for n in ordered_names}:
            continue
        actual = available.get(key)
        if actual is None:
            missing_plugins.append(name)
            continue
        header = read_plugin_header(os.path.join(data_root, actual))
        headers[key] = header
        ordered_names.append(header.name)

    if missing_plugins:
        # A missing entry that is not required by anyone is only an error when
        # it was marked active; otherwise it is a stale loadorder line.
        active_missing = [n for n in missing_plugins
                          if not plugin_entries or n.lower() in active_names]
        if active_missing:
            raise MissingPluginError(
                "active plugin(s) missing from Data: %s"
                % ", ".join(sorted(active_missing, key=str.lower)))
    missing_set = {n.lower() for n in missing_plugins}

    # Force masters of active plugins active.  A master listed later is moved
    # before its dependent by the DFS below.
    effective_active = {n for n in active_names if n in headers}
    for key, header in headers.items():
        if key in effective_active:
            for master in header.masters:
                effective_active.add(master.lower())

    state: Dict[str, int] = {}  # 1 = visiting, 2 = done
    result: List[LoadedPlugin] = []

    def visit(key: str, stack: Tuple[str, ...]) -> None:
        status = state.get(key, 0)
        if status == 2:
            return
        if status == 1:
            cycle = " -> ".join(stack + (headers[key].name,))
            raise PluginOrderError("master cycle: %s" % cycle)
        header = headers.get(key)
        if header is None:
            return
        state[key] = 1
        for master in header.masters:
            master_key = master.lower()
            if master_key not in headers and master_key in available:
                headers[master_key] = read_plugin_header(
                    os.path.join(data_root, available[master_key]))
                effective_active.add(master_key)
            if master_key in missing_set or master_key not in headers:
                raise MissingMasterError(master, header.name)
            effective_active.add(master_key)
            visit(master_key, stack + (header.name,))
        state[key] = 2
        if key in effective_active:
            result.append(LoadedPlugin(header, True, len(declared)))

    for key in [k for k, h in headers.items() if k in effective_active]:
        visit(key, ())

    # Any declared-but-inactive plugin is reported, never mounted.
    inactive = [h.name for k, h in headers.items() if k not in effective_active]
    report = {
        "data_root": os.path.abspath(data_root),
        "plugins_txt": plugins_path,
        "loadorder_txt": loadorder_path,
        "discovery_note": discovered_note,
        "declared": declared,
        "active": [p.name for p in result],
        "inactive": sorted(inactive, key=str.lower),
        "missing_plugins": sorted(missing_plugins, key=str.lower),
    }
    return result, report


# ---------------------------------------------------------------------------
# Overlay
# ---------------------------------------------------------------------------


@dataclass(frozen=True)
class ArchiveMount:
    priority: int
    plugin: str
    host_path: str
    name: str

    def as_dict(self) -> dict:
        return {
            "priority": self.priority,
            "plugin": self.plugin,
            "name": self.name,
            "path": self.host_path,
            "exists": os.path.isfile(self.host_path),
        }


@dataclass(frozen=True)
class ResolvedAsset:
    virtual_path: str
    kind: str  # "loose" | "bsa"
    host_path: str
    priority: int
    plugin: str = ""
    archive: str = ""
    entry_path: str = ""

    def as_dict(self) -> dict:
        return {
            "virtual_path": self.virtual_path,
            "kind": self.kind,
            "host_path": self.host_path,
            "priority": self.priority,
            "plugin": self.plugin,
            "archive": self.archive,
            "entry_path": self.entry_path,
        }


def _archive_names_for(plugin: str, data_root: str) -> List[str]:
    lower = plugin.lower()
    if lower in OFFICIAL_ARCHIVES:
        return [name for name in OFFICIAL_ARCHIVES[lower]
                if os.path.isfile(os.path.join(data_root, name))]
    stem = os.path.splitext(plugin)[0]
    listing = [n for n in os.listdir(data_root)
               if n.lower().endswith(ARCHIVE_EXTENSION)
               and os.path.isfile(os.path.join(data_root, n))]
    matches = [n for n in listing
               if n.lower() == (stem + ".bsa").lower()
               or n.lower().startswith((stem + " - ").lower())]
    main = [n for n in matches if n.lower() == (stem + ".bsa").lower()]
    extras = sorted((n for n in matches if n not in main), key=str.lower)
    return main + extras


class DataOverlay:
    """Deterministic Data view used by the runtime loader and the scanners."""

    def __init__(self, data_root: Optional[str] = None, *,
                 plugins_path: Optional[str] = None,
                 loadorder_path: Optional[str] = None,
                 auto_discover: bool = True,
                 env: Optional[Mapping[str, str]] = None):
        self.data_root = resolve_data_root(data_root, env)
        self.load_order, self.load_report = build_load_order(
            self.data_root, plugins_path, loadorder_path, auto_discover)
        mounts: List[ArchiveMount] = []
        priority = 0
        seen = set()
        for loaded in self.load_order:
            for name in _archive_names_for(loaded.name, self.data_root):
                host = os.path.join(self.data_root, name)
                key = os.path.normcase(os.path.abspath(host))
                if key in seen:
                    # Re-mounting the same archive at a later priority is the
                    # TESV behaviour; keep only the highest-priority instance.
                    mounts = [m for m in mounts
                              if os.path.normcase(os.path.abspath(m.host_path)) != key]
                seen.add(key)
                mounts.append(ArchiveMount(priority, loaded.name, host, name))
                priority += 1
        self.archives = mounts
        self._archives: Dict[str, bsa.BsaArchive] = {}
        self._dir_cache: Dict[str, Dict[str, str]] = {}
        self._loose_calls = 0

    # -- lifecycle -----------------------------------------------------

    def close(self) -> None:
        for archive in self._archives.values():
            archive.close()
        self._archives.clear()

    def __enter__(self) -> "DataOverlay":
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        self.close()

    # -- loose files ---------------------------------------------------

    def _find_loose(self, virtual_path: str) -> Optional[str]:
        norm = normalize_virtual_path(virtual_path)
        parts = norm.split("\\")
        directory = os.path.join(self.data_root, *parts[:-1])
        cache_key = os.path.normcase(os.path.abspath(directory))
        listing = self._dir_cache.get(cache_key)
        if listing is None:
            try:
                entries = os.listdir(directory)
            except OSError:
                entries = []
            listing = {name.lower(): name for name in entries}
            # Do not cache a miss for a directory that did not exist yet: a
            # loose file may be created after the first resolve (tests and
            # mod installation both do this).
            if entries or os.path.isdir(directory):
                self._dir_cache[cache_key] = listing
        actual = listing.get(parts[-1].lower())
        if actual is None:
            return None
        path = os.path.join(directory, actual)
        return path if os.path.isfile(path) else None

    def loose_files(self) -> Iterable[str]:
        """Loose asset paths (plugins/archives excluded), deterministic order."""
        skip_ext = PLUGIN_EXTENSIONS + (".bsa", ".ba2", ".txt")
        for root, dirs, files in os.walk(self.data_root):
            dirs.sort(key=str.lower)
            for name in sorted(files, key=str.lower):
                if name.lower().endswith(skip_ext):
                    continue
                full = os.path.join(root, name)
                rel = os.path.relpath(full, self.data_root)
                yield rel

    # -- BSA -----------------------------------------------------------

    def _open(self, mount: ArchiveMount) -> bsa.BsaArchive:
        key = os.path.normcase(mount.host_path)
        archive = self._archives.get(key)
        if archive is not None:
            return archive
        try:
            archive = bsa.BsaArchive(mount.host_path)
        except (bsa.BsaError, OSError) as exc:
            raise ArchiveError("cannot mount '%s' for %s: %s"
                               % (mount.host_path, mount.plugin, exc)) from exc
        self._archives[key] = archive
        return archive

    def resolve(self, virtual_path: str) -> Optional[ResolvedAsset]:
        """Highest-priority source, or ``None`` when no source has the path."""
        norm = normalize_virtual_path(virtual_path)
        loose = self._find_loose(norm)
        if loose is not None:
            return ResolvedAsset(norm, "loose", loose, 1 << 30)
        for mount in reversed(self.archives):
            if not os.path.isfile(mount.host_path):
                continue
            archive = self._open(mount)
            entry = archive.find(norm)
            if entry is not None:
                return ResolvedAsset(norm, "bsa", mount.host_path, mount.priority,
                                     mount.plugin, mount.name, entry.path)
        return None

    def validate(self) -> dict:
        """Open every mounted archive and return its header summary."""
        rows = []
        for mount in self.archives:
            if not os.path.isfile(mount.host_path):
                rows.append({"archive": mount.name, "plugin": mount.plugin,
                             "exists": False})
                continue
            archive = self._open(mount)
            header = archive.header
            rows.append({
                "archive": mount.name,
                "plugin": mount.plugin,
                "priority": mount.priority,
                "exists": True,
                "version": header.version,
                "compressed": header.compressed,
                "embedded_names": header.embedded_names,
                "folders": header.folder_count,
                "files": header.file_count,
                "index_bytes": archive.index_bytes()
                if hasattr(archive, "index_bytes") else None,
            })
        return {"data_root": self.data_root, "archives": rows}

    def manifest(self) -> dict:
        return {
            "data_root": self.data_root,
            "load_order": self.load_report,
            "plugins": [p.as_dict() for p in self.load_order],
            "archives": [m.as_dict() for m in self.archives],
        }
