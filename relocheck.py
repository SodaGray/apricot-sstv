#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""relocheck.py APP.reloc.elf -- will this app survive the NanoApps loader?

mkrelocapp.py packs only R_ARM_ABS32 relocations; the loader adds the load base
to those words and nothing else. Any other absolute relocation (MOVW/MOVT
pairs, ABS16, TARGET1 ...) is silently left at its link-time (base 0) value,
and so is an absolute address inside a linker-generated veneer (veneers have
no relocations of their own). Either one means a wrong address at run time.
Exit status 1, with the offending functions listed, if anything like that is
in the image; 0 otherwise.
"""
import sys, struct
from elftools.elf.elffile import ELFFile
from elftools.elf.constants import SH_FLAGS
from elftools.elf.relocation import RelocationSection

ABS32 = 2
NAMES = {43: 'R_ARM_MOVW_ABS_NC', 44: 'R_ARM_MOVT_ABS', 47: 'R_ARM_THM_MOVW_ABS_NC',
         48: 'R_ARM_THM_MOVT_ABS', 5: 'R_ARM_ABS16', 8: 'R_ARM_ABS8', 38: 'R_ARM_TARGET1'}
# position-independent within the blob: branches, PC-relative data, unwind
PCREL = {1, 3, 4, 10, 11, 28, 29, 30, 40, 42, 45, 46, 49, 50, 51, 52, 53, 54,
         102, 103} | set(range(57, 87))

elf = ELFFile(open(sys.argv[1], 'rb'))
funcs = sorted((s['st_value'] & ~1, s['st_size'], s.name)
               for s in elf.get_section_by_name('.symtab').iter_symbols()
               if s['st_info']['type'] == 'STT_FUNC' and s['st_value'])
def where(off):
    name = '?'
    for a, n, nm in funcs:
        if a <= off: name = nm
        else: break
    return name

bad = {}
abs32_at = set()
for sec in elf.iter_sections():
    if not isinstance(sec, RelocationSection): continue
    tgt = elf.get_section(sec['sh_info'])
    if tgt is None or not (tgt['sh_flags'] & SH_FLAGS.SHF_ALLOC): continue
    for r in sec.iter_relocations():
        t = r['r_info_type']
        if t == ABS32: abs32_at.add(r['r_offset'])
        elif t not in PCREL:
            bad.setdefault((where(r['r_offset']), t), 0)
            bad[(where(r['r_offset']), t)] += 1

# linker veneers/stubs: a literal word in a stub that is not covered by an
# ABS32 relocation and points into the image would be left unrelocated
span = max(s['sh_addr'] + s['sh_size'] for s in elf.iter_sections() if s['sh_flags'] & SH_FLAGS.SHF_ALLOC)
text = elf.get_section_by_name('.text')
tdata, taddr = text.data(), text['sh_addr']
veneers = [(a, n, nm) for a, n, nm in funcs if nm.endswith('_veneer') or '_from_arm' in nm or '_from_thumb' in nm]
syms = [s for s in elf.get_section_by_name('.symtab').iter_symbols()]
for s in syms:
    nm = s.name
    if (nm.endswith('_veneer') or '_from_arm' in nm or '_from_thumb' in nm) and s['st_value']:
        a = s['st_value'] & ~1
        for off in range(a & ~3, a + 16, 4):
            if off + 4 > taddr + len(tdata) or off < taddr: continue
            w = struct.unpack_from('<I', tdata, off - taddr)[0]
            if 0 < w < span and off not in abs32_at:
                bad.setdefault((nm, 'absolute literal in veneer'), 0)
                bad[(nm, 'absolute literal in veneer')] += 1

if bad:
    print("relocheck: %s has addresses the NanoApps loader will not relocate:" % sys.argv[1])
    for (fn, t), n in sorted(bad.items(), key=str):
        print("  %-28s %s x%d" % (fn, NAMES.get(t, 'R_ARM type %d' % t) if isinstance(t, int) else t, n))
    print("  (MOVW/MOVT: link a libm/libgcc built without them, such as the arm/v5te")
    print("   multilib, or compile your own code with -mword-relocations)")
    sys.exit(1)
print("relocheck: ok (%d ABS32 relocations, %d veneers checked)" % (len(abs32_at), len(veneers)))
