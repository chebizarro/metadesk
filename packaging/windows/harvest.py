#!/usr/bin/env python3
"""Deterministic WiX 4/5 per-user components with HKCU keypaths."""
import argparse
import hashlib
from pathlib import Path
import uuid
import xml.etree.ElementTree as ET

NS = 'http://wixtoolset.org/schemas/v4/wxs'
ET.register_namespace('', NS)
SPACE = uuid.UUID('8288c82c-c6fc-4860-96c1-14489d1a6662')


def ident(prefix, relative):
    return prefix + hashlib.sha256(relative.encode()).hexdigest()[:24]


def add(parent, tag, **attrs):
    return ET.SubElement(parent, '{' + NS + '}' + tag, attrs)


def harvest(stage, output):
    root = ET.Element('{' + NS + '}Wix')
    fragment = add(root, 'Fragment')
    group = add(fragment, 'ComponentGroup', Id='Payload')
    dirs = {'.': add(fragment, 'DirectoryRef', Id='INSTALLDIR')}
    for path in sorted(stage.rglob('*')):
        rel = path.relative_to(stage).as_posix()
        parent = path.parent.relative_to(stage).as_posix()
        if path.is_dir():
            dirs[rel] = add(dirs[parent], 'Directory', Id=ident('d', rel), Name=path.name)
            cid = ident('c', 'dir:' + rel)
            component = add(dirs[rel], 'Component', Id=cid, Guid=str(uuid.uuid5(SPACE, 'dir:' + rel)).upper())
            add(component, 'CreateFolder')
            add(component, 'RemoveFolder', Id=ident('r', rel), On='uninstall')
            add(component, 'RegistryValue', Root='HKCU', Key='Software\\Metadesk.Installer\\Folders',
                Name=ident('v', rel), Type='integer', Value='1', KeyPath='yes')
            add(group, 'ComponentRef', Id=cid)
            continue
        cid = ident('c', rel)
        component = add(dirs[parent], 'Component', Id=cid, Guid=str(uuid.uuid5(SPACE, rel)).upper())
        add(component, 'File', Id=ident('f', rel), Source=str(path.resolve()), KeyPath='no')
        add(component, 'RegistryValue', Root='HKCU', Key='Software\\Metadesk.Installer\\Files',
            Name=ident('v', rel), Type='integer', Value='1', KeyPath='yes')
        add(group, 'ComponentRef', Id=cid)
    ET.indent(root)
    ET.ElementTree(root).write(output, encoding='utf-8', xml_declaration=True)


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('stage', type=Path)
    p.add_argument('output', type=Path)
    a = p.parse_args()
    harvest(a.stage, a.output)
