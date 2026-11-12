#!/usr/bin/env python3
"""Disassemble an EA APT (compiled Flash/ActionScript) movie from the user's
own dump.

The front end is EA's "APT" player -- the same bytecode family documented by
the OpenSAGE project for C&C Generals / Zero Hour and Battle for Middle-earth
(github.com/OpenSAGE/OpenSAGE, src/OpenSage.Game/Data/Apt and
src/OpenSage.Game/Gui/Apt/ActionScript). When the C side hands a movie its
loader variables and nothing happens, the useful question is not "did the
interpreter run" but "what did the movie's own bytecode do with what it was
given" -- and that is answerable from the .apt/.const pair alone, offline,
without instrumenting the running game.

    python scripts/apt-dump.py screens/screens.viv screens/feloader.big
    python scripts/apt-dump.py screens/screens.viv screens/feloader.big --frames
    python scripts/apt-dump.py screens/screens.viv screens/feloader.big --actions

Paths are relative to $DEFJAM_DATA/extracted, same convention as
scripts/big-entry.py and scripts/refpack.py (whose decompress() and
big_entries() this tool imports and reuses rather than reimplementing).

With no flags: prints the movie header (screen size, frame rate, counts) and
one line per character (index, type, and for sprites/buttons their own frame
or action counts).

--frames: additionally lists, for the main timeline and every sprite
character, each frame's label (if any) and frame items -- placed objects
(depth, character, name, flags), removed objects, background colour, and
which frames carry an Action or InitAction block or a placed clip's clip
events (onEnterFrame, onLoad, ...), each tagged with the flag bits and its
byte offset in the .apt file.

--actions: additionally disassembles every action block found anywhere in
the movie (frame actions, init actions, placed-clip events, button actions,
sprite actions) to opcode mnemonics and operands, resolving string operands
and constant-pool references against the .const file. Each block is labelled
with where it was found and its starting file offset, so a disassembled
instruction can be correlated with an interpreter dispatch address at
runtime.

Format notes (reverse-engineered from the bytes plus the OpenSAGE C# source,
since Def Jam is not one of OpenSAGE's supported titles): the .apt file's
internal pointers are plain absolute byte offsets from the start of the .apt
stream -- nothing needs relocation to read them. The .const file is a flat
array of typed constant entries (strings, ints, floats, booleans, registers)
referenced by index. An action's ConstantPool instruction (if present) lists
*indices into that array*, and later opcodes such as EA_PushConstantByte or
EA_CallNamedFunc reference that instruction's list by a small local index,
not the .const file directly -- this tool tracks that per action block, the
same way the reference VM does.

Hygiene: this tool prints structure, names, strings and opcodes only. It
never writes any game byte to disk, anywhere, under any flag.
"""

import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import refpack  # decompress(), big_entries() -- reused, not reimplemented


# ---------------------------------------------------------------- constants

class ConstEntry:
    __slots__ = ("type", "value")

    def __init__(self, type_, value):
        self.type = type_
        self.value = value

    def display(self):
        if self.type == 1:
            return repr(self.value)
        if self.type == 2:
            return "property#%r" % (self.value,)
        if self.type == 3:
            return "<none>"
        if self.type == 4:
            return "register#%d" % self.value
        if self.type == 5:
            return "true" if self.value else "false"
        if self.type == 6:
            return "%g" % self.value
        if self.type == 7:
            return "%d" % self.value
        if self.type == 8:
            return "lookup#%d" % self.value
        return "?(type=%d)%r" % (self.type, self.value)


CONST_TYPE_NAMES = {
    0: "Undef", 1: "String", 2: "Property", 3: "None", 4: "Register",
    5: "Boolean", 6: "Float", 7: "Integer", 8: "Lookup",
}


def read_cstr(buf, off):
    end = buf.find(b"\x00", off)
    if end < 0:
        end = len(buf)
    return buf[off:end].decode("latin-1")


def parse_const(data):
    """(entry_offset, [ConstEntry, ...]) from a .const file's bytes."""
    magic = data[:17].decode("latin-1", "replace")
    if magic != "Apt constant file":
        raise SystemExit("not a supported const file: %r" % magic)
    pos = 17 + 3  # 3-byte gap after the fixed-length magic, per ConstantData.cs
    entry_offset, num_entries, header_size = struct.unpack_from("<III", data, pos)
    pos += 12
    if header_size != 32:
        raise SystemExit("constant header must be 32 bytes, got %d" % header_size)

    entries = []
    for _ in range(num_entries):
        etype = struct.unpack_from("<I", data, pos)[0]
        pos += 4
        if etype == 0:
            raise SystemExit("undefined const entry at file offset 0x%X" % (pos - 4))
        elif etype == 1:  # String
            str_off = struct.unpack_from("<I", data, pos)[0]
            pos += 4
            value = read_cstr(data, str_off)
        elif etype == 2:  # Property -- format unconfirmed by OpenSAGE (TODO in upstream)
            value = struct.unpack_from("<I", data, pos)[0]
            pos += 4
        elif etype == 3:  # None -- no payload at all
            value = None
        elif etype == 4:  # Register
            value = struct.unpack_from("<I", data, pos)[0]
            pos += 4
        elif etype == 5:  # Boolean (1 byte + 3 must-be-zero pad, per ReadBooleanUInt32Checked)
            b = data[pos]
            pad = int.from_bytes(data[pos + 1:pos + 4], "little")
            if b not in (0, 1) or pad != 0:
                raise SystemExit("bad boolean const entry at 0x%X" % pos)
            value = bool(b)
            pos += 4
        elif etype == 6:  # Float
            value = struct.unpack_from("<f", data, pos)[0]
            pos += 4
        elif etype == 7:  # Integer
            value = struct.unpack_from("<i", data, pos)[0]
            pos += 4
        elif etype == 8:  # Lookup
            value = struct.unpack_from("<I", data, pos)[0]
            pos += 4
        else:
            raise SystemExit("unknown const entry type %d at 0x%X" % (etype, pos - 4))
        entries.append(ConstEntry(etype, value))
    return entry_offset, entries


# -------------------------------------------------------------- ActionScript

# InstructionType enum, from OpenSAGE's Gui/Apt/ActionScript/Opcodes/Instruction.cs.
# Names match exactly; this is the full enum regardless of whether OpenSAGE's
# own parser implements every one of them (see OPERAND below).
OPNAMES = {
    0x00: "End", 0x04: "NextFrame", 0x05: "PrevFrame", 0x06: "Play", 0x07: "Stop",
    0x08: "ToggleQuality", 0x09: "StopSounds", 0x0A: "Add", 0x0B: "Subtract",
    0x0C: "Multiply", 0x0D: "Divide", 0x0E: "Equals", 0x0F: "LessThan",
    0x10: "And", 0x11: "Or", 0x12: "Not", 0x13: "StringEquals",
    0x14: "StringLength", 0x15: "SubString", 0x17: "Pop", 0x18: "ToInteger",
    0x1C: "GetVariable", 0x1D: "SetVariable", 0x20: "SetTarget2",
    0x21: "StringConcat", 0x22: "GetProperty", 0x23: "SetProperty",
    0x24: "CloneSprite", 0x25: "RemoveSprite", 0x26: "Trace",
    0x27: "StartDragMovie", 0x28: "StopDragMovie", 0x29: "StringCompare",
    0x2A: "Throw", 0x2B: "CastOp", 0x2C: "ImplementsOp", 0x30: "Random",
    0x31: "MbLength", 0x32: "Ord", 0x33: "Chr", 0x34: "GetTimer",
    0x35: "MbSubString", 0x36: "MbOrd", 0x37: "MbChr", 0x3A: "Delete",
    0x3B: "Delete2", 0x3C: "DefineLocal", 0x3D: "CallFunction", 0x3E: "Return",
    0x3F: "Modulo", 0x40: "NewObject", 0x41: "Var", 0x42: "InitArray",
    0x43: "InitObject", 0x44: "TypeOf", 0x45: "TargetPath", 0x46: "Enumerate",
    0x47: "Add2", 0x48: "LessThan2", 0x49: "Equals2", 0x4A: "ToNumber",
    0x4B: "ToString", 0x4C: "PushDuplicate", 0x4D: "StackSwap",
    0x4E: "GetMember", 0x4F: "SetMember", 0x50: "Increment", 0x51: "Decrement",
    0x52: "CallMethod", 0x53: "NewMethod", 0x54: "InstanceOf",
    0x55: "Enumerate2", 0x56: "EA_PushThis", 0x58: "EA_PushGlobal",
    0x59: "EA_PushZero", 0x5A: "EA_PushOne", 0x5B: "EA_CallFuncPop",
    0x5C: "EA_CallFunc", 0x5D: "EA_CallMethodPop", 0x5E: "EA_CallMethod",
    0x60: "BitwiseAnd", 0x61: "BitwiseOr", 0x62: "BitwiseXOr",
    0x63: "ShiftLeft", 0x64: "ShiftRight", 0x65: "ShiftRight2",
    0x66: "StrictEqual", 0x67: "Greater", 0x68: "StringGreater",
    0x69: "Extends", 0x70: "EA_PushThisVar", 0x71: "EA_PushGlobalVar",
    0x72: "EA_ZeroVar", 0x73: "EA_PushTrue", 0x74: "EA_PushFalse",
    0x75: "EA_PushNull", 0x76: "EA_PushUndefined", 0x77: "TraceStart",
    0x81: "GotoFrame", 0x83: "GetURL", 0x87: "SetRegister",
    0x88: "ConstantPool", 0x8A: "WaitFormFrame", 0x8B: "SetTarget",
    0x8C: "GotoLabel", 0x8D: "WaitForFrameExpr", 0x8E: "DefineFunction2",
    0x8F: "Try", 0x94: "With", 0x96: "PushData", 0x99: "BranchAlways",
    0x9A: "GetURL2", 0x9B: "DefineFunction", 0x9D: "BranchIfTrue",
    0x9E: "CallFrame", 0x9F: "GotoFrame2", 0xA1: "EA_PushString",
    0xA2: "EA_PushConstantByte", 0xA3: "EA_PushConstantWord",
    0xA4: "EA_GetStringVar", 0xA5: "EA_GetStringMember",
    0xA6: "EA_SetStringVar", 0xA7: "EA_SetStringMember",
    0xAE: "EA_PushValueOfVar", 0xAF: "EA_GetNamedMember",
    0xB0: "EA_CallNamedFuncPop", 0xB1: "EA_CallNamedFunc",
    0xB2: "EA_CallNamedMethodPop", 0xB3: "EA_CallNamedMethod",
    0xB4: "EA_PushFloat", 0xB5: "EA_PushByte", 0xB6: "EA_PushShort",
    0xB7: "EA_PushLong", 0xB8: "EA_BranchIfFalse", 0xB9: "EA_PushRegister",
    0xFF: "Padding",
}

# Opcodes requiring 4-byte alignment of the stream *before* their operand is
# read, per OpenSAGE's InstructionAlignment.IsAligned.
ALIGNED = {
    "DefineFunction", "DefineFunction2", "ConstantPool", "BranchIfTrue",
    "BranchAlways", "PushData", "GetURL", "GotoLabel", "SetRegister",
    "SetTarget", "GotoFrame", "GotoFrame2", "With", "EA_PushString",
    "EA_BranchIfFalse", "EA_GetStringVar", "EA_SetStringVar",
    "EA_GetStringMember", "EA_SetStringMember",
}

# Opcodes whose exact operand encoding is confirmed by OpenSAGE's
# InstructionCollection.Parse (the reference decoder). Anything not listed
# here has no confirmed encoding -- OpenSAGE's own parser would throw
# "Unimplemented bytecode instruction" on it. If Def Jam's bytecode uses one
# of those, this tool stops disassembling that block rather than guess.
#
# EA_CallMethod (0x5E) is the one exception: OpenSAGE's switch has no case
# for it at all (it handles EA_CallFunc/EA_CallFuncPop/EA_CallMethodPop --
# all no-operand, stack-based -- but never EA_CallMethod itself), so it is
# not "confirmed" by that source in the strict sense. It is included here
# because feloader's own bytecode (Action @0xB154, file offset 0xC175)
# settles the question directly: treating it as a bare opcode with no
# operand bytes -- the same shape as its CallMethod/EA_CallMethodPop
# siblings -- keeps the stream byte-aligned for the rest of the block,
# including a later BranchIfTrue whose target (0xC1AA) lands exactly on a
# decoded EA_PushThisVar rather than mid-instruction garbage.
CONFIRMED = {
    "End", "NextFrame", "Play", "Stop", "Add", "Subtract", "Multiply",
    "Divide", "Not", "StringEquals", "Pop", "ToInteger", "ToNumber",
    "GetVariable", "SetVariable", "StringConcat", "GetProperty",
    "SetProperty", "Trace", "Random", "Delete", "Delete2", "DefineLocal",
    "CallFunction", "Return", "Modulo", "NewObject", "Var", "InitArray",
    "InitObject", "TypeOf", "Add2", "LessThan2", "Equals2", "ToString",
    "PushDuplicate", "GetMember", "SetMember", "Increment", "Decrement",
    "CallMethod", "Enumerate2", "EA_PushThis", "EA_PushZero", "EA_PushOne",
    "EA_CallFunc", "EA_CallMethodPop", "BitwiseXOr", "Greater",
    "EA_PushThisVar", "EA_PushGlobalVar", "EA_ZeroVar", "EA_PushTrue",
    "EA_PushFalse", "EA_PushNull", "EA_PushUndefined", "GotoFrame", "GetURL",
    "SetRegister", "ConstantPool", "GotoLabel", "DefineFunction2", "PushData",
    "BranchAlways", "GetURL2", "DefineFunction", "BranchIfTrue", "GotoFrame2",
    "EA_PushString", "EA_PushConstantByte", "EA_GetStringVar",
    "EA_SetStringVar", "EA_GetStringMember", "EA_SetStringMember",
    "EA_PushValueOfVar", "EA_GetNamedMember", "EA_CallNamedFuncPop",
    "EA_CallNamedFunc", "EA_CallNamedMethodPop", "EA_PushFloat",
    "EA_PushByte", "EA_PushShort", "EA_CallNamedMethod", "EA_PushRegister",
    "EA_PushConstantWord", "EA_CallFuncPop", "StrictEqual", "Extends",
    "InstanceOf", "EA_CallMethod",
}


def align4(pos):
    rem = pos % 4
    return pos if rem == 0 else pos + (4 - rem)


def read_string_at_offset(buf, pos):
    off = struct.unpack_from("<I", buf, pos)[0]
    return read_cstr(buf, off), pos + 4


class Instr:
    __slots__ = ("offset", "name", "opcode", "operands", "note", "branch_target")

    def __init__(self, offset, name, opcode, operands, note="", branch_target=None):
        self.offset = offset
        self.name = name
        self.opcode = opcode
        self.operands = operands
        self.note = note
        self.branch_target = branch_target


def parse_instruction(buf, pos, const_entries, local_pool):
    """One instruction at pos. Returns (Instr, next_pos, is_end, is_confirmed)."""
    start = pos
    opcode = buf[pos]
    pos += 1
    name = OPNAMES.get(opcode, "opcode_0x%02X" % opcode)

    if name in ALIGNED:
        pos = align4(pos)

    # SWF's rule, which APT keeps: an action code below 0x80 has no operand
    # bytes. RemoveSprite (0x25) in battleID's frame 0 was the first of these
    # outside OpenSAGE's parser; stopping there lost the rest of the frame.
    if name not in CONFIRMED and opcode >= 0x80:
        return Instr(start, name, opcode, [], "UNCONFIRMED ENCODING"), pos, False, False

    operands = []
    branch_target = None

    def resolve_local(idx):
        if 0 <= idx < len(local_pool):
            gidx = local_pool[idx]
            if 0 <= gidx < len(const_entries):
                return "const[%d]=%s" % (gidx, const_entries[gidx].display())
            return "const[%d]=<out of range>" % gidx
        return "pool[%d]=<no such local constant>" % idx

    if name == "GotoFrame":
        frame = struct.unpack_from("<i", buf, pos)[0]
        pos += 4
        operands.append("frame=%d" % frame)
    elif name == "GetURL":
        url, pos = read_string_at_offset(buf, pos)
        target, pos = read_string_at_offset(buf, pos)
        operands.append("url=%r" % url)
        operands.append("target=%r" % target)
    elif name == "SetRegister":
        reg = struct.unpack_from("<i", buf, pos)[0]
        pos += 4
        operands.append("register=%d" % reg)
    elif name == "ConstantPool":
        count = struct.unpack_from("<I", buf, pos)[0]
        pos += 4
        list_off = struct.unpack_from("<I", buf, pos)[0]
        pos += 4
        ids = struct.unpack_from("<%dI" % count, buf, list_off) if count else ()
        local_pool[:] = ids
        for i, gidx in enumerate(ids):
            disp = const_entries[gidx].display() if gidx < len(const_entries) else "<out of range>"
            operands.append("[%d]=const[%d]=%s" % (i, gidx, disp))
    elif name == "GotoLabel":
        label, pos = read_string_at_offset(buf, pos)
        operands.append("label=%r" % label)
    elif name == "PushData":
        count = struct.unpack_from("<I", buf, pos)[0]
        pos += 4
        list_off = struct.unpack_from("<I", buf, pos)[0]
        pos += 4
        ids = struct.unpack_from("<%dI" % count, buf, list_off) if count else ()
        for gidx in ids:
            disp = const_entries[gidx].display() if gidx < len(const_entries) else "<out of range>"
            operands.append("const[%d]=%s" % (gidx, disp))
    elif name == "BranchAlways" or name == "BranchIfTrue":
        off = struct.unpack_from("<i", buf, pos)[0]
        pos += 4
        branch_target = pos + off
        operands.append("offset=%d -> file_offset=0x%X" % (off, branch_target))
    elif name == "GetURL2":
        pass  # stack-based, no inline operand
    elif name == "DefineFunction2":
        fname, pos = read_string_at_offset(buf, pos)
        n_params = struct.unpack_from("<I", buf, pos)[0]
        pos += 4
        n_regs = buf[pos]
        pos += 1
        flags = int.from_bytes(buf[pos:pos + 3], "little")
        pos += 3
        list_off = struct.unpack_from("<I", buf, pos)[0]
        pos += 4
        p = list_off
        params = []
        for _ in range(n_params):
            reg = struct.unpack_from("<i", buf, p)[0]
            pname, _ = read_string_at_offset(buf, p + 4)
            params.append("r%d:%r" % (reg, pname))
            p += 8
        body_size = struct.unpack_from("<I", buf, pos)[0]
        pos += 4
        pos += 8  # reserved
        operands.append("name=%r registers=%d flags=0x%X params=(%s) body_size=%d body_at=0x%X" %
                         (fname, n_regs, flags, ", ".join(params), body_size, pos))
    elif name == "DefineFunction":
        fname, pos = read_string_at_offset(buf, pos)
        n_params = struct.unpack_from("<I", buf, pos)[0]  # = the param list's own capacity field
        pos += 4
        list_off = struct.unpack_from("<I", buf, pos)[0]
        pos += 4
        params = []
        p = list_off
        for _ in range(n_params):
            pname = read_cstr(buf, struct.unpack_from("<I", buf, p)[0])
            params.append(pname)
            p += 4
        body_size = struct.unpack_from("<I", buf, pos)[0]
        pos += 4
        pos += 8  # reserved
        operands.append("name=%r params=(%s) body_size=%d body_at=0x%X" %
                         (fname, ", ".join(params), body_size, pos))
    elif name == "GotoFrame2":
        flags = struct.unpack_from("<i", buf, pos)[0]
        pos += 4
        operands.append("play=%s" % bool(flags & 1))
    elif name == "EA_PushString":
        s, pos = read_string_at_offset(buf, pos)
        operands.append("string=%r" % s)
    elif name == "EA_PushConstantByte":
        idx = buf[pos]
        pos += 1
        operands.append(resolve_local(idx))
    elif name in ("EA_GetStringVar", "EA_SetStringVar", "EA_GetStringMember", "EA_SetStringMember"):
        s, pos = read_string_at_offset(buf, pos)
        operands.append("name=%r" % s)
    elif name == "EA_PushValueOfVar":
        idx = buf[pos]
        pos += 1
        operands.append(resolve_local(idx))
    elif name == "EA_GetNamedMember":
        idx = buf[pos]
        pos += 1
        operands.append("member=" + resolve_local(idx))
    elif name in ("EA_CallNamedFuncPop", "EA_CallNamedFunc", "EA_CallNamedMethodPop", "EA_CallNamedMethod"):
        idx = buf[pos]
        pos += 1
        operands.append("name=" + resolve_local(idx))
    elif name == "EA_PushFloat":
        val = struct.unpack_from("<f", buf, pos)[0]
        pos += 4
        operands.append("value=%g" % val)
    elif name == "EA_PushByte":
        val = buf[pos]
        pos += 1
        operands.append("value=%d" % val)
    elif name == "EA_PushShort":
        val = struct.unpack_from("<H", buf, pos)[0]
        pos += 2
        operands.append("value=%d" % val)
    elif name == "EA_PushRegister":
        reg = buf[pos]
        pos += 1
        operands.append("register=%d" % reg)
    elif name == "EA_PushConstantWord":
        idx = struct.unpack_from("<H", buf, pos)[0]
        pos += 2
        operands.append(resolve_local(idx))
    # else: no-operand opcode, nothing more to read

    return Instr(start, name, opcode, operands, branch_target=branch_target), pos, name == "End", True


def disassemble_block(buf, start_pos, const_entries, max_bytes=None):
    """[Instr, ...] starting at start_pos, mirroring OpenSAGE's CanParse:
    keep going while the stream hasn't hit an End past every branch target
    seen so far. Stops early (with a trailing note) on an unconfirmed
    opcode, since its true size is unknown and guessing risks desyncing the
    rest of the block."""
    instrs = []
    local_pool = []
    pos = start_pos
    furthest = start_pos
    limit = len(buf) if max_bytes is None else min(len(buf), start_pos + max_bytes)

    while True:
        if not instrs or instrs[-1].name != "End" or pos <= furthest:
            if pos >= limit:
                break
            instr, next_pos, is_end, confirmed = parse_instruction(buf, pos, const_entries, local_pool)
            instrs.append(instr)
            if not confirmed:
                break
            if instr.branch_target is not None:
                furthest = max(furthest, instr.branch_target)
            pos = next_pos
        else:
            break

    return instrs


def print_disasm(instrs, indent="    "):
    for ins in instrs:
        opnd = ", ".join(ins.operands)
        note = ("  ; " + ins.note) if ins.note else ""
        print("%s0x%06X  %-22s %s%s" % (indent, ins.offset, ins.name, opnd, note))


# ------------------------------------------------------------------- movie

CHAR_TYPE_NAMES = {
    1: "Shape", 2: "Text", 3: "Font", 4: "Button", 5: "Sprite", 6: "Sound",
    7: "Image", 8: "Morph", 9: "Movie", 10: "StaticText", 11: "None", 12: "Video",
}

FRAME_ITEM_NAMES = {1: "Action", 2: "FrameLabel", 3: "PlaceObject", 4: "RemoveObject",
                     5: "BackgroundColor", 8: "InitAction"}

PLACE_FLAGS = [("Move", 1), ("HasCharacter", 2), ("HasMatrix", 4),
               ("HasColorTransform", 8), ("HasRatio", 16), ("HasName", 32),
               ("HasClipDepth", 64), ("HasClipAction", 128)]

CLIP_EVENT_FLAGS = [
    (0x800000, "KeyUp"), (0x400000, "KeyDown"), (0x200000, "MouseUp"),
    (0x100000, "MouseDown"), (0x080000, "MouseMove"), (0x040000, "Unload"),
    (0x020000, "EnterFrame"), (0x010000, "Load"), (0x008000, "DragOver"),
    (0x004000, "RollOut"), (0x002000, "RollOver"), (0x001000, "ReleaseOutside"),
    (0x000800, "Release"), (0x000400, "Press"), (0x000200, "DragOut"),
    (0x000100, "Data"), (0x000004, "Construct"), (0x000002, "KeyPress"),
    (0x000001, "Initialize"),
]


def clip_flag_names(flags):
    return [n for bit, n in CLIP_EVENT_FLAGS if flags & bit] or ["(none)"]


def parse_clip_events(buf, pos):
    """PlaceObject's optional clip-action list: count/offset header, then
    ClipEvent records (flags:u24, keycode:u8, offset_to_next:u32,
    instructions_at:u32) at that offset."""
    capacity, list_off = struct.unpack_from("<iI", buf, pos)
    events = []
    p = list_off
    for _ in range(capacity):
        flags = int.from_bytes(buf[p:p + 3], "little")
        keycode = buf[p + 3]
        instr_pos = struct.unpack_from("<I", buf, p + 8)[0]
        events.append((flags, keycode, instr_pos))
        p += 12
    return events


def parse_place_object(buf, pos):
    flags = struct.unpack_from("<I", buf, pos)[0]
    depth = struct.unpack_from("<i", buf, pos + 4)[0]
    pos2 = pos + 8
    character = struct.unpack_from("<i", buf, pos2)[0] if flags & 2 else None
    pos2 += 4
    # Matrix2x2 (4 floats) + Vector2 (2 floats), a slot whether present or not.
    matrix = struct.unpack_from("<6f", buf, pos2) if flags & 4 else None
    pos2 += 24
    if flags & 8:
        pos2 += 8  # two ColorRgba
    else:
        pos2 += 8
    pos2 += 4  # ratio float (present or not, still a slot)
    name = None
    if flags & 32:
        name, pos2 = read_string_at_offset(buf, pos2)
    else:
        pos2 += 4
    clip_depth = struct.unpack_from("<i", buf, pos2)[0]
    pos2 += 4
    clip_events = []
    if flags & 128:
        poa_off = struct.unpack_from("<I", buf, pos2)[0]
        pos2 += 4
        if poa_off:
            clip_events = parse_clip_events(buf, poa_off)
    return {
        "flags": flags, "depth": depth, "character": character, "matrix": matrix,
        "name": name, "clip_depth": clip_depth, "clip_events": clip_events,
    }


def parse_frame_item(buf, pos):
    """(kind_name, data_dict, item_end_hint) for one FrameItem at pos.
    item_end_hint is unused for navigation (items are reached individually
    via the frame's own pointer list) but kept for readability."""
    kind = struct.unpack_from("<I", buf, pos)[0]
    body = pos + 4
    name = FRAME_ITEM_NAMES.get(kind, "Unknown(%d)" % kind)
    data = {}
    if name == "Action":
        data["instructions_at"] = struct.unpack_from("<I", buf, body)[0]
    elif name == "InitAction":
        data["sprite"] = struct.unpack_from("<I", buf, body)[0]
        data["instructions_at"] = struct.unpack_from("<I", buf, body + 4)[0]
    elif name == "FrameLabel":
        label, p = read_string_at_offset(buf, body)
        flags, frame_id = struct.unpack_from("<II", buf, p)
        data["name"] = label
        data["flags"] = flags
        data["frame_id"] = frame_id
    elif name == "PlaceObject":
        data.update(parse_place_object(buf, body))
    elif name == "RemoveObject":
        data["depth"] = struct.unpack_from("<i", buf, body)[0]
    elif name == "BackgroundColor":
        data["rgba"] = tuple(buf[body:body + 4])
    return name, data


def parse_frames_array(buf, list_pos):
    """capacity/offset header at list_pos -> [ (frame_items_ptr_header), ... ]
    Each Frame record is 8 bytes: an (capacity, offset) header for its own
    pointer-list of FrameItems."""
    capacity, list_off = struct.unpack_from("<iI", buf, list_pos)
    frames = []
    p = list_off
    for _ in range(capacity):
        item_capacity, item_list_off = struct.unpack_from("<iI", buf, p)
        items = []
        for i in range(item_capacity):
            ptr = struct.unpack_from("<I", buf, item_list_off + i * 4)[0]
            if ptr:
                items.append(parse_frame_item(buf, ptr))
        frames.append(items)
        p += 8
    return frames


def parse_character(buf, pos, want_full=True):
    """Character record at absolute offset pos. Returns a dict; for
    Sprite/Button/Movie it recurses into their own frames/actions."""
    ctype, sig = struct.unpack_from("<II", buf, pos)
    body = pos + 8
    tname = CHAR_TYPE_NAMES.get(ctype, "Unknown(%d)" % ctype)
    rec = {"offset": pos, "type": tname, "sig_ok": sig == 0x09876543}

    if not want_full:
        return rec

    if tname == "Movie":
        rec["frames"] = parse_frames_array(buf, body)
        chars_hdr = body + 8 + 4  # frames(8) + unknown(4)
        capacity, list_off = struct.unpack_from("<iI", buf, chars_hdr)
        rec["screen_width"], rec["screen_height"], rec["ms_per_frame"] = \
            struct.unpack_from("<III", buf, chars_hdr + 8)
        rec["_characters_ptr_list"] = (capacity, list_off)
    elif tname == "Sprite":
        rec["frames"] = parse_frames_array(buf, body)
    elif tname == "Button":
        actions_hdr = body + 4 + 16 + 4 + 4 + 4 + 4 + 8
        # IsMenu(4) Bounds(16) tc(4) vc(4) Vertices-hdr(4) Triangles-hdr(4) Records-hdr(8)
        capacity, list_off = struct.unpack_from("<iI", buf, actions_hdr)
        actions = []
        p = list_off
        for _ in range(capacity):
            aflags = buf[p]
            keycode = struct.unpack_from("<H", buf, p + 1)[0]
            instr_pos = struct.unpack_from("<I", buf, p + 4)[0]
            actions.append({"flags": aflags, "keycode": keycode, "instructions_at": instr_pos})
            p += 8
        rec["button_actions"] = actions

    return rec


def read_characters_list(buf, movie_rec):
    capacity, list_off = movie_rec["_characters_ptr_list"]
    chars = []
    for i in range(capacity):
        ptr = struct.unpack_from("<I", buf, list_off + i * 4)[0]
        if ptr == 0:
            chars.append(None)
            continue
        chars.append(parse_character(buf, ptr))
    return chars


# ------------------------------------------------------------------ loading

def load_movie(archive_rel, entry_rel):
    root = os.path.join(os.environ.get("DEFJAM_DATA", ""), "extracted")
    archive_path = os.path.join(root, archive_rel.replace("/", os.sep))
    if not os.path.exists(archive_path):
        raise SystemExit("no such archive: " + archive_path)

    with open(archive_path, "rb") as fh:
        head = fh.read(4 * 1024 * 1024)
        table = refpack.big_entries(head)
        if table is None:
            raise SystemExit("outer file is not a BIG archive")
        for name, off, size in table:
            if name == entry_rel or name.endswith("/" + entry_rel.split("/")[-1]):
                fh.seek(off)
                blob = fh.read(size)
                break
        else:
            raise SystemExit("no entry named %r" % entry_rel)

    inner = refpack.decompress(blob)
    inner_table = refpack.big_entries(inner)
    if inner_table is None:
        raise SystemExit("decompressed entry is not itself a BIG archive")

    base = os.path.splitext(os.path.basename(entry_rel))[0]
    apt_bytes = const_bytes = None
    for n, o, s in inner_table:
        if n == base + ".apt":
            apt_bytes = inner[o:o + s]
        elif n == base + ".const":
            const_bytes = inner[o:o + s]
    if apt_bytes is None or const_bytes is None:
        raise SystemExit("could not find %s.apt/.const in %s" % (base, entry_rel))

    magic = apt_bytes[:8].decode("latin-1", "replace")
    if magic != "Apt Data":
        raise SystemExit("not a supported apt file: %r" % magic)

    entry_offset, const_entries = parse_const(const_bytes)
    movie_rec = parse_character(apt_bytes, entry_offset)
    if movie_rec["type"] != "Movie":
        raise SystemExit("entry offset does not point at a Movie character (got %s)" % movie_rec["type"])
    characters = read_characters_list(apt_bytes, movie_rec)
    return apt_bytes, const_entries, movie_rec, characters, base


# --------------------------------------------------------------------- CLI

def describe_flags(rec):
    return ", ".join(n for n, bit in PLACE_FLAGS if rec["flags"] & bit) or "(none)"


def print_frame_items(buf, frames, const_entries, do_actions, label):
    for fi, items in enumerate(frames):
        if not items:
            continue
        print("  frame %d (%s):" % (fi, label))
        for kind, data in items:
            if kind == "FrameLabel":
                print("    FrameLabel name=%r flags=0x%X frame_id=%d" %
                      (data["name"], data["flags"], data["frame_id"]))
            elif kind == "PlaceObject":
                extra = ""
                if data["clip_events"]:
                    ce_desc = "; ".join(
                        "%s@0x%X" % ("/".join(clip_flag_names(f)), ip)
                        for f, kc, ip in data["clip_events"])
                    extra = "  clip_events=[%s]" % ce_desc
                if data.get("matrix"):
                    extra += "  matrix=[%s]" % " ".join("%.4g" % v for v in data["matrix"])
                print("    PlaceObject depth=%d char=%s name=%r flags=[%s]%s" %
                      (data["depth"], data["character"], data["name"],
                       describe_flags(data), extra))
                if do_actions:
                    for f, kc, ip in data["clip_events"]:
                        print("      -- clip event %s (key=%d) @0x%X --" %
                              ("/".join(clip_flag_names(f)), kc, ip))
                        instrs = disassemble_block(buf, ip, const_entries)
                        print_disasm(instrs, indent="        ")
            elif kind == "RemoveObject":
                print("    RemoveObject depth=%d" % data["depth"])
            elif kind == "BackgroundColor":
                print("    BackgroundColor rgba=%s" % (data["rgba"],))
            elif kind == "Action":
                ip = data["instructions_at"]
                print("    Action @0x%X" % ip)
                if do_actions:
                    instrs = disassemble_block(buf, ip, const_entries)
                    print_disasm(instrs)
            elif kind == "InitAction":
                ip = data["instructions_at"]
                print("    InitAction sprite=%d @0x%X" % (data["sprite"], ip))
                if do_actions:
                    instrs = disassemble_block(buf, ip, const_entries)
                    print_disasm(instrs)


def main():
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="backslashreplace")
    except AttributeError:
        pass
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("archive", help="outer BIG4 archive, e.g. screens/screens.viv")
    ap.add_argument("entry", help="RefPack .big entry inside it, e.g. screens/feloader.big")
    ap.add_argument("--frames", action="store_true", help="list frame items per frame")
    ap.add_argument("--actions", action="store_true", help="disassemble every action block (implies --frames detail for actions)")
    args = ap.parse_args()

    buf, const_entries, movie_rec, characters, base = load_movie(args.archive, args.entry)

    print("%s: %d frames, %dx%d, %d ms/frame, %d constants, %d characters" % (
        base, len(movie_rec["frames"]), movie_rec["screen_width"], movie_rec["screen_height"],
        movie_rec["ms_per_frame"], len(const_entries), len(characters)))
    print()
    print("characters:")
    for i, c in enumerate(characters):
        if c is None:
            print("  [%3d] <null>" % i)
            continue
        extra = ""
        if c["type"] == "Sprite":
            extra = " frames=%d" % len(c["frames"])
        elif c["type"] == "Button":
            extra = " actions=%d" % len(c.get("button_actions", []))
        elif c["type"] == "Movie":
            extra = " (self)"
        print("  [%3d] %-10s off=0x%06X sig_ok=%s%s" % (i, c["type"], c["offset"], c["sig_ok"], extra))

    if args.frames or args.actions:
        print()
        print("main timeline (%s):" % base)
        print_frame_items(buf, movie_rec["frames"], const_entries, args.actions, "main")

        for i, c in enumerate(characters):
            if c is None:
                continue
            if c["type"] == "Sprite":
                print()
                print("sprite character [%d] timeline:" % i)
                print_frame_items(buf, c["frames"], const_entries, args.actions, "sprite %d" % i)
            elif c["type"] == "Button" and c.get("button_actions"):
                print()
                print("button character [%d] actions:" % i)
                for a in c["button_actions"]:
                    print("  flags=0x%02X keycode=%d @0x%X" % (a["flags"], a["keycode"], a["instructions_at"]))
                    if args.actions:
                        instrs = disassemble_block(buf, a["instructions_at"], const_entries)
                        print_disasm(instrs)


if __name__ == "__main__":
    main()
