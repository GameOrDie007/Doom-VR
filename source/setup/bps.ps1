<#
    Apply a BPS patch - the format DOOM 64 CE ships its DOOM64.WAD patch in -
    without running anyone's patching program.

        . .\bps.ps1
        Apply-Bps -Source DOOM64.WAD -Patch DOOM64.bps -Target DOOM64.IWAD

    BPS (byuu's "beat" patch format, public specification): "BPS1", the
    source size, target size and metadata as variable-length numbers, then
    commands until the last 12 bytes, which are the CRC32s of the source, the
    target and the patch itself. All three are checked, so a wrong source file
    (a modified or different release of the WAD) or a damaged patch is refused
    instead of producing a broken game file.

    Written for Doom VR.

    Copyright (C) 2026 Game Or Die. All rights reserved. May be
    redistributed only as part of Doom VR, unchanged.
#>

if (-not ("DoomVR.Bps" -as [type])) {
    Add-Type -TypeDefinition @"
using System;
using System.IO;
namespace DoomVR {
public static class Bps {
    static uint[] table;
    public static uint Crc32(byte[] data, int count) {
        if (table == null) {
            table = new uint[256];
            for (uint i = 0; i < 256; i++) {
                uint c = i;
                for (int k = 0; k < 8; k++) c = (c & 1) != 0 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                table[i] = c;
            }
        }
        uint crc = 0xFFFFFFFFu;
        for (int i = 0; i < count; i++) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
        return crc ^ 0xFFFFFFFFu;
    }
    static ulong Decode(byte[] p, ref int at) {
        ulong data = 0, shift = 1;
        while (true) {
            byte x = p[at++];
            data += (ulong)(x & 0x7f) * shift;
            if ((x & 0x80) != 0) break;
            shift <<= 7;
            data += shift;
        }
        return data;
    }
    static uint U32(byte[] p, int at) {
        return (uint)(p[at] | p[at + 1] << 8 | p[at + 2] << 16 | p[at + 3] << 24);
    }
    // Returns "" on success, or why the patch was refused.
    public static string Apply(string sourcePath, string patchPath, string targetPath) {
        byte[] src = File.ReadAllBytes(sourcePath);
        byte[] p = File.ReadAllBytes(patchPath);
        if (p.Length < 19 || p[0] != 'B' || p[1] != 'P' || p[2] != 'S' || p[3] != '1') return "not a BPS patch";
        int footer = p.Length - 12;
        if (Crc32(p, p.Length - 4) != U32(p, footer + 8)) return "the patch file is damaged (its checksum does not match)";
        if (Crc32(src, src.Length) != U32(p, footer)) return "this is not the file the patch was made for (checksum differs) - a different release, or modified";
        int at = 4;
        ulong srcSize = Decode(p, ref at), dstSize = Decode(p, ref at), metaSize = Decode(p, ref at);
        at += (int)metaSize;
        if ((ulong)src.Length != srcSize) return "the source file is the wrong size";
        byte[] dst = new byte[dstSize];
        long outAt = 0, srcRel = 0, dstRel = 0;
        while (at < footer) {
            ulong data = Decode(p, ref at);
            long length = (long)(data >> 2) + 1;
            switch ((int)(data & 3)) {
                case 0: // SourceRead
                    Array.Copy(src, outAt, dst, outAt, length); outAt += length; break;
                case 1: // TargetRead
                    Array.Copy(p, at, dst, outAt, length); at += (int)length; outAt += length; break;
                case 2: { // SourceCopy
                    ulong d = Decode(p, ref at);
                    srcRel += ((d & 1) != 0 ? -1 : 1) * (long)(d >> 1);
                    Array.Copy(src, srcRel, dst, outAt, length); srcRel += length; outAt += length; break; }
                default: { // TargetCopy - may overlap itself, so byte by byte
                    ulong d = Decode(p, ref at);
                    dstRel += ((d & 1) != 0 ? -1 : 1) * (long)(d >> 1);
                    for (long i = 0; i < length; i++) dst[outAt++] = dst[dstRel++];
                    break; }
            }
        }
        if ((ulong)outAt != dstSize) return "the patch did not produce the whole file";
        if (Crc32(dst, dst.Length) != U32(p, footer + 4)) return "the patched result is wrong (checksum differs)";
        File.WriteAllBytes(targetPath, dst);
        return "";
    }
}
}
"@
}

function Apply-Bps([string]$Source, [string]$Patch, [string]$Target) {
    return [DoomVR.Bps]::Apply($Source, $Patch, $Target)
}
