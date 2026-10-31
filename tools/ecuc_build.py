"""ecuc_build.py - tiny builder that writes ECUC configuration values (.arxml) from Python.

Used for the stand-in demo ECU and for the OS unit-test configuration so that the real
ECUC parser (tools/ecuc.py) and generator (tools/gen_os_cfg.py) are exercised by CI.
"""
from __future__ import annotations

from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional, Sequence, Union
from xml.sax.saxutils import escape

RefList = Sequence[str]


class Cont:
    def __init__(self, defn: str, name: str, params: Optional[Dict[str, Any]] = None,
                 refs: Optional[Dict[str, Union[str, RefList]]] = None, subs: Optional[Iterable["Cont"]] = None) -> None:
        self.defn = defn
        self.name = name
        self.params = dict(params or {})
        self.refs = dict(refs or {})
        self.subs = list(subs or [])

    def add(self, sub: "Cont") -> "Cont":
        self.subs.append(sub)
        return self


class EcucModule:
    def __init__(self, module: str, defroot: str, package: str = "ActiveEcuC") -> None:
        self.module = module
        self.defroot = defroot.rstrip("/")
        self.package = package
        self.containers: List[Cont] = []

    def add(self, c: Cont) -> Cont:
        self.containers.append(c)
        return c

    # ------------------------------------------------------------------
    def _refpath(self, target: str) -> str:
        if target.startswith("/"):
            return target
        return f"/{self.package}/{self.module}/{target}"

    def _emit_container(self, c: Cont, defpath: str, out: List[str], indent: str) -> None:
        out.append(f'{indent}<ECUC-CONTAINER-VALUE>')
        out.append(f'{indent}  <SHORT-NAME>{escape(c.name)}</SHORT-NAME>')
        out.append(f'{indent}  <DEFINITION-REF DEST="ECUC-PARAM-CONF-CONTAINER-DEF">{defpath}</DEFINITION-REF>')
        if c.params:
            out.append(f'{indent}  <PARAMETER-VALUES>')
            for k, v in c.params.items():
                values = v if isinstance(v, (list, tuple)) else [v]
                for val in values:
                    if isinstance(val, bool):
                        kind, dest, txt = "ECUC-NUMERICAL-PARAM-VALUE", "ECUC-BOOLEAN-PARAM-DEF", "true" if val else "false"
                    elif isinstance(val, int):
                        kind, dest, txt = "ECUC-NUMERICAL-PARAM-VALUE", "ECUC-INTEGER-PARAM-DEF", str(val)
                    elif isinstance(val, float):
                        kind, dest, txt = "ECUC-NUMERICAL-PARAM-VALUE", "ECUC-FLOAT-PARAM-DEF", repr(val)
                    else:
                        kind, dest, txt = "ECUC-TEXTUAL-PARAM-VALUE", "ECUC-ENUMERATION-PARAM-DEF", str(val)
                    out.append(f'{indent}    <{kind}>')
                    out.append(f'{indent}      <DEFINITION-REF DEST="{dest}">{defpath}/{k}</DEFINITION-REF>')
                    out.append(f'{indent}      <VALUE>{escape(txt)}</VALUE>')
                    out.append(f'{indent}    </{kind}>')
            out.append(f'{indent}  </PARAMETER-VALUES>')
        if c.refs:
            out.append(f'{indent}  <REFERENCE-VALUES>')
            for k, v in c.refs.items():
                targets = [v] if isinstance(v, str) else list(v)
                for t in targets:
                    out.append(f'{indent}    <ECUC-REFERENCE-VALUE>')
                    out.append(f'{indent}      <DEFINITION-REF DEST="ECUC-REFERENCE-DEF">{defpath}/{k}</DEFINITION-REF>')
                    out.append(f'{indent}      <VALUE-REF DEST="ECUC-CONTAINER-VALUE">{self._refpath(t)}</VALUE-REF>')
                    out.append(f'{indent}    </ECUC-REFERENCE-VALUE>')
            out.append(f'{indent}  </REFERENCE-VALUES>')
        if c.subs:
            out.append(f'{indent}  <SUB-CONTAINERS>')
            for s in c.subs:
                self._emit_container(s, f"{defpath}/{s.defn}", out, indent + "    ")
            out.append(f'{indent}  </SUB-CONTAINERS>')
        out.append(f'{indent}</ECUC-CONTAINER-VALUE>')

    def emit_module(self, out: List[str]) -> None:
        out.append('        <ECUC-MODULE-CONFIGURATION-VALUES>')
        out.append(f'          <SHORT-NAME>{self.module}</SHORT-NAME>')
        out.append(f'          <DEFINITION-REF DEST="ECUC-MODULE-DEF">{self.defroot}</DEFINITION-REF>')
        out.append('          <CONTAINERS>')
        for c in self.containers:
            self._emit_container(c, f"{self.defroot}/{c.defn}", out, "            ")
        out.append('          </CONTAINERS>')
        out.append('        </ECUC-MODULE-CONFIGURATION-VALUES>')

    def to_xml(self) -> str:
        return modules_xml([self])

    def write(self, path: Union[str, Path]) -> None:
        Path(path).write_text(self.to_xml(), encoding="utf-8", newline="\n")


def modules_xml(modules: Sequence["EcucModule"]) -> str:
    """One arxml with several modules in the package of the first one."""
    out: List[str] = []
    out.append('<?xml version="1.0" encoding="UTF-8" standalone="no"?>')
    out.append('<AUTOSAR xmlns="http://autosar.org/schema/r4.0" xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance" '
               'xsi:schemaLocation="http://autosar.org/schema/r4.0 AUTOSAR_00049.xsd">')
    out.append('  <AR-PACKAGES>')
    out.append('    <AR-PACKAGE>')
    out.append(f'      <SHORT-NAME>{modules[0].package}</SHORT-NAME>')
    out.append('      <ELEMENTS>')
    for m in modules:
        m.emit_module(out)
    out.append('      </ELEMENTS>')
    out.append('    </AR-PACKAGE>')
    out.append('  </AR-PACKAGES>')
    out.append('</AUTOSAR>')
    return "\n".join(out) + "\n"


def write_modules(path: Union[str, Path], modules: Sequence["EcucModule"]) -> None:
    Path(path).write_text(modules_xml(modules), encoding="utf-8", newline="\n")
