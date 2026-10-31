"""ecuc.py - minimal reader for AUTOSAR ECUC configuration values (.arxml).

Only the standard ECUC value elements are interpreted:
ECUC-MODULE-CONFIGURATION-VALUES, ECUC-CONTAINER-VALUE, ECUC-NUMERICAL-PARAM-VALUE,
ECUC-TEXTUAL-PARAM-VALUE, ECUC-REFERENCE-VALUE (+ instance/choice references), SUB-CONTAINERS.
Containers are matched by the last segment of their DEFINITION-REF, which is the AUTOSAR
container name (OsTask, OsAlarm, ...). Works with DaVinci Configurator exports.
"""
from __future__ import annotations

import xml.etree.ElementTree as ET
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, Iterable, Iterator, List, Optional

NS = "{http://autosar.org/schema/r4.0}"


def _tag(name: str) -> str:
    return NS + name


def _text(e: Optional[ET.Element], default: str = "") -> str:
    return e.text.strip() if (e is not None and e.text is not None) else default


@dataclass
class Container:
    name: str
    defpath: str
    path: str
    params: Dict[str, List[str]] = field(default_factory=dict)
    refs: Dict[str, List[str]] = field(default_factory=dict)
    subs: List["Container"] = field(default_factory=list)
    parent: Optional["Container"] = None

    @property
    def defn(self) -> str:
        return self.defpath.rsplit("/", 1)[-1]

    # --- parameter access -------------------------------------------------
    def get(self, pname: str, default: Optional[str] = None) -> Optional[str]:
        v = self.params.get(pname)
        return v[0] if v else default

    def getall(self, pname: str) -> List[str]:
        return list(self.params.get(pname, []))

    def getint(self, pname: str, default: int = 0) -> int:
        v = self.get(pname)
        if v is None or v == "":
            return default
        try:
            return int(v, 0)
        except ValueError:
            return int(float(v))

    def getfloat(self, pname: str, default: float = 0.0) -> float:
        v = self.get(pname)
        return float(v) if v not in (None, "") else default

    def getbool(self, pname: str, default: bool = False) -> bool:
        v = self.get(pname)
        if v is None:
            return default
        return v.strip().lower() in ("true", "1")

    def refpaths(self, rname: str) -> List[str]:
        return list(self.refs.get(rname, []))

    def ref(self, rname: str) -> List[str]:
        """Referenced containers' short names (last path segment)."""
        return [p.rsplit("/", 1)[-1] for p in self.refs.get(rname, [])]

    def ref1(self, rname: str, default: Optional[str] = None) -> Optional[str]:
        r = self.ref(rname)
        return r[0] if r else default

    def sub(self, defn: str) -> List["Container"]:
        return [s for s in self.subs if s.defn == defn]

    def sub1(self, defn: str) -> Optional["Container"]:
        s = self.sub(defn)
        return s[0] if s else None

    def walk(self) -> Iterator["Container"]:
        yield self
        for s in self.subs:
            yield from s.walk()


@dataclass
class Module:
    name: str
    defpath: str
    path: str
    containers: List[Container] = field(default_factory=list)

    def all(self) -> Iterator[Container]:
        for c in self.containers:
            yield from c.walk()

    def find(self, defn: str) -> List[Container]:
        """All containers (any depth) whose definition name is defn, in file order."""
        return [c for c in self.all() if c.defn == defn]

    def top(self, defn: str) -> List[Container]:
        return [c for c in self.containers if c.defn == defn]

    def byname(self, name: str) -> Optional[Container]:
        for c in self.all():
            if c.name == name:
                return c
        return None


class Ecuc:
    def __init__(self) -> None:
        self.modules: Dict[str, Module] = {}
        self.bypath: Dict[str, Container] = {}

    @staticmethod
    def load(paths: Iterable[str | Path]) -> "Ecuc":
        e = Ecuc()
        for p in paths:
            e._load_file(Path(p))
        return e

    def _load_file(self, path: Path) -> None:
        root = ET.parse(str(path)).getroot()
        for pkg in root.iter(_tag("AR-PACKAGE")):
            pkgname = _text(pkg.find(_tag("SHORT-NAME")))
            elements = pkg.find(_tag("ELEMENTS"))
            if elements is None:
                continue
            for mod in elements.findall(_tag("ECUC-MODULE-CONFIGURATION-VALUES")):
                mname = _text(mod.find(_tag("SHORT-NAME")))
                mdef = _text(mod.find(_tag("DEFINITION-REF")))
                mpath = f"/{pkgname}/{mname}"
                module = self.modules.get(mname)
                if module is None:
                    module = Module(mname, mdef, mpath)
                    self.modules[mname] = module
                conts = mod.find(_tag("CONTAINERS"))
                if conts is None:
                    continue
                for c in conts.findall(_tag("ECUC-CONTAINER-VALUE")):
                    module.containers.append(self._parse_container(c, mpath, None))

    def _parse_container(self, el: ET.Element, parentpath: str, parent: Optional[Container]) -> Container:
        name = _text(el.find(_tag("SHORT-NAME")))
        defpath = _text(el.find(_tag("DEFINITION-REF")))
        path = f"{parentpath}/{name}"
        c = Container(name=name, defpath=defpath, path=path, parent=parent)
        pv = el.find(_tag("PARAMETER-VALUES"))
        if pv is not None:
            for p in pv:
                pdef = _text(p.find(_tag("DEFINITION-REF"))).rsplit("/", 1)[-1]
                val = _text(p.find(_tag("VALUE")))
                c.params.setdefault(pdef, []).append(val)
        rv = el.find(_tag("REFERENCE-VALUES"))
        if rv is not None:
            for r in rv:
                rdef = _text(r.find(_tag("DEFINITION-REF"))).rsplit("/", 1)[-1]
                vr = r.find(_tag("VALUE-REF"))
                if vr is None:
                    # instance references: CONTEXT/TARGET
                    vr = r.find(_tag("VALUE") + "/" + _tag("TARGET-REF"))
                c.refs.setdefault(rdef, []).append(_text(vr))
        sc = el.find(_tag("SUB-CONTAINERS"))
        if sc is not None:
            for s in sc.findall(_tag("ECUC-CONTAINER-VALUE")):
                c.subs.append(self._parse_container(s, path, c))
        self.bypath[path] = c
        return c

    def module(self, name: str) -> Module:
        if name not in self.modules:
            raise KeyError(f"module {name} not found; available: {sorted(self.modules)}")
        return self.modules[name]

    def resolve(self, refpath: str) -> Optional[Container]:
        return self.bypath.get(refpath)


if __name__ == "__main__":  # small CLI for inspection
    import sys

    e = Ecuc.load(sys.argv[1:2])
    for m in e.modules.values():
        print(f"module {m.name} ({m.defpath}): {len(m.containers)} top-level containers")
        kinds: Dict[str, int] = {}
        for c in m.all():
            kinds[c.defn] = kinds.get(c.defn, 0) + 1
        for k, v in sorted(kinds.items()):
            print(f"  {k}: {v}")
