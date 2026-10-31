<#
    Covers and the splash for DoomVR, made on the player's machine from the
    player's own game files - nothing drawn by anyone else ships.

        . .\art.ps1
        $title = [DoomVR.Art]::FindTitle([string[]]$filesInLoadOrder)
        [DoomVR.Art]::MakeCover($title, "Doom II", "out.png")
        [DoomVR.Art]::MakeSplash($titles, "DOOM VR", "vrsplash.png")

    FindTitle looks, from the last file loaded back to the IWAD (so a mod's own
    title screen wins), for TITLEPIC - or TITLE, which Heretic and Hexen use -
    in WADs and in PK3/IPK3 archives, and draws it with the palette those files
    carry. It reads the formats the engine does: a Doom picture (columns of
    posts, including "tall" patches), a raw 320x200 screen, or a PNG/JPEG.
    320x200 art is shown 320x240, as the games' pixels are taller than wide.

    Written for Doom VR from the Doom WAD and picture formats (the
    Unofficial Doom Specs) and the ZIP format.

    Copyright (C) 2026 Game Or Die. All rights reserved. May be
    redistributed only as part of Doom VR, unchanged.
#>

if (-not ("DoomVR.Art" -as [type])) {
    Add-Type -ReferencedAssemblies System.Drawing, System.IO.Compression, System.IO.Compression.FileSystem -TypeDefinition @"
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.Drawing.Text;
using System.IO;
using System.IO.Compression;
namespace DoomVR {
public static class Art {
    // ---- reading lumps -------------------------------------------------
    static byte[] WadLump(string path, string name) {
        using (var fs = File.OpenRead(path)) using (var br = new BinaryReader(fs)) {
            string magic = new string(br.ReadChars(4));
            if (magic != "IWAD" && magic != "PWAD") return null;
            int count = br.ReadInt32(), dir = br.ReadInt32();
            if (count <= 0 || dir <= 0 || dir > fs.Length) return null;
            fs.Position = dir;
            byte[] found = null;
            for (int i = 0; i < count; i++) {
                int pos = br.ReadInt32(), size = br.ReadInt32();
                string n = System.Text.Encoding.ASCII.GetString(br.ReadBytes(8)).TrimEnd('\0').ToUpperInvariant();
                if (n == name && size > 0 && pos + (long)size <= fs.Length) {
                    long back = fs.Position; fs.Position = pos; found = br.ReadBytes(size); fs.Position = back;
                }
            }
            return found; // the last one, as the engine would take it
        }
    }
    static byte[] ZipLump(string path, string name) {
        try {
            using (var z = ZipFile.OpenRead(path)) {
                ZipArchiveEntry best = null;
                foreach (var e in z.Entries) {
                    string full = e.FullName.Replace('\\', '/').ToLowerInvariant();
                    string dir = full.Contains("/") ? full.Substring(0, full.LastIndexOf('/')) : "";
                    string file = Path.GetFileNameWithoutExtension(full);
                    if (file != name.ToLowerInvariant()) continue;
                    if (dir != "" && dir != "graphics") continue;
                    best = e;
                }
                if (best == null) return null;
                using (var s = best.Open()) using (var m = new MemoryStream()) { s.CopyTo(m); return m.ToArray(); }
            }
        } catch { return null; }
    }
    static byte[] Lump(string file, string name) {
        try {
            using (var fs = File.OpenRead(file)) {
                byte[] h = new byte[4]; if (fs.Read(h, 0, 4) < 4) return null;
                if (h[0] == 'P' && h[1] == 'K') { fs.Close(); return ZipLump(file, name); }
            }
            return WadLump(file, name);
        } catch { return null; }
    }

    // ---- drawing lumps -------------------------------------------------
    static Color[] Palette(byte[] playpal) {
        var p = new Color[256];
        for (int i = 0; i < 256; i++)
            p[i] = playpal != null && playpal.Length >= 768 ? Color.FromArgb(playpal[i * 3], playpal[i * 3 + 1], playpal[i * 3 + 2]) : Color.FromArgb(i, i, i);
        return p;
    }
    static Bitmap Decode(byte[] d, Color[] pal) {
        if (d == null || d.Length < 8) return null;
        // PNG or JPEG
        if ((d[0] == 0x89 && d[1] == 'P') || (d[0] == 0xFF && d[1] == 0xD8)) {
            try { using (var m = new MemoryStream(d)) return new Bitmap(Image.FromStream(m)); } catch { return null; }
        }
        // A raw full screen: Heretic's and Hexen's TITLE.
        if (d.Length == 64000) {
            var b = new Bitmap(320, 200);
            for (int y = 0; y < 200; y++) for (int x = 0; x < 320; x++) b.SetPixel(x, y, pal[d[y * 320 + x]]);
            return b;
        }
        // A Doom picture: columns of posts.
        int w = BitConverter.ToInt16(d, 0), h = BitConverter.ToInt16(d, 2);
        if (w <= 0 || h <= 0 || w > 4096 || h > 4096 || 8 + w * 4 > d.Length) return null;
        var bmp = new Bitmap(w, h);
        using (var g = Graphics.FromImage(bmp)) g.Clear(Color.Black);
        for (int x = 0; x < w; x++) {
            int at = BitConverter.ToInt32(d, 8 + x * 4);
            int top = -1;
            while (at >= 0 && at < d.Length) {
                int delta = d[at];
                if (delta == 0xFF) break;
                top = delta <= top ? top + delta : delta; // tall patches
                if (at + 2 >= d.Length) break;
                int len = d[at + 1];
                for (int i = 0; i < len && at + 3 + i < d.Length; i++) {
                    int y = top + i;
                    if (y >= 0 && y < h) bmp.SetPixel(x, y, pal[d[at + 3 + i]]);
                }
                at += len + 4;
            }
        }
        return bmp;
    }

    // The title screen of a game made of these files, in load order (IWAD
    // first). Null when none of them has one.
    public static Bitmap FindTitle(string[] files) {
        byte[] playpal = null;
        for (int i = files.Length - 1; i >= 0 && playpal == null; i--) playpal = Lump(files[i], "PLAYPAL");
        var pal = Palette(playpal);
        for (int i = files.Length - 1; i >= 0; i--) {
            foreach (string n in new[] { "TITLEPIC", "TITLE" }) {
                var bmp = Decode(Lump(files[i], n), pal);
                if (bmp != null && bmp.Width >= 64 && bmp.Height >= 64) return bmp;
            }
        }
        return null;
    }

    // ---- composing -------------------------------------------------------
    static float Shown(Bitmap t) { return t.Height == 200 ? 1.2f : 1f; } // Doom's tall pixels
    static void Cover(Graphics g, Bitmap src, RectangleF dst) {
        // Scale to fill dst, cropping the overflow, centred.
        float sh = src.Height * Shown(src);
        float s = Math.Max(dst.Width / src.Width, dst.Height / sh);
        float w = src.Width * s, h = sh * s;
        g.DrawImage(src, dst.X + (dst.Width - w) / 2, dst.Y + (dst.Height - h) / 2, w, h);
    }
    static Bitmap Blur(Bitmap src, int w, int h) {
        // Down to a thumbnail and back up, smoothly: a cheap, strong blur.
        var small = new Bitmap(Math.Max(1, w / 24), Math.Max(1, h / 24));
        using (var g = Graphics.FromImage(small)) { g.InterpolationMode = InterpolationMode.HighQualityBilinear; Cover(g, src, new RectangleF(0, 0, small.Width, small.Height)); }
        var big = new Bitmap(w, h);
        using (var g = Graphics.FromImage(big)) { g.InterpolationMode = InterpolationMode.HighQualityBicubic; g.DrawImage(small, -8, -8, w + 16, h + 16); }
        small.Dispose();
        return big;
    }
    static void Text(Graphics g, string s, RectangleF box, float size, Color fill, bool bold) {
        var fam = new FontFamily("Arial");
        foreach (var f in FontFamily.Families) if (f.Name == "Segoe UI Black") { fam = f; break; }
        var fmt = new StringFormat { Alignment = StringAlignment.Center, LineAlignment = StringAlignment.Center };
        // Shrink until it fits.
        for (; size > 10; size -= 2) {
            using (var font = new Font(fam, size, bold ? FontStyle.Bold : FontStyle.Regular, GraphicsUnit.Pixel)) {
                var need = g.MeasureString(s, font, (int)box.Width, fmt);
                if (need.Height <= box.Height) break;
            }
        }
        using (var path = new GraphicsPath()) {
            path.AddString(s, fam, (int)(bold ? FontStyle.Bold : FontStyle.Regular), size, box, fmt);
            using (var pen = new Pen(Color.FromArgb(230, 0, 0, 0), Math.Max(3f, size / 9f)) { LineJoin = LineJoin.Round }) g.DrawPath(pen, path);
            using (var br = new SolidBrush(fill)) g.FillPath(br, path);
        }
    }

    public static void MakeCover(Bitmap title, string name, string outPath) {
        const int W = 600, H = 900;
        using (var bmp = new Bitmap(W, H)) using (var g = Graphics.FromImage(bmp)) {
            g.SmoothingMode = SmoothingMode.AntiAlias;
            g.TextRenderingHint = TextRenderingHint.AntiAliasGridFit;
            g.Clear(Color.FromArgb(18, 8, 8));
            if (title != null) {
                using (var bg = Blur(title, W, H)) g.DrawImage(bg, 0, 0);
                using (var shade = new SolidBrush(Color.FromArgb(150, 0, 0, 0))) g.FillRectangle(shade, 0, 0, W, H);
                // The title screen itself, whole, in the upper part.
                float sh = title.Height * Shown(title);
                float s = Math.Min(560f / title.Width, 470f / sh);
                float w = title.Width * s, h = sh * s;
                float x = (W - w) / 2, y = 95 + (470 - h) / 2;
                g.InterpolationMode = InterpolationMode.NearestNeighbor;
                g.PixelOffsetMode = PixelOffsetMode.Half;
                using (var edge = new SolidBrush(Color.FromArgb(200, 0, 0, 0))) g.FillRectangle(edge, x - 6, y - 6, w + 12, h + 12);
                g.DrawImage(title, x, y, w, h);
                g.InterpolationMode = InterpolationMode.HighQualityBicubic;
            } else {
                using (var lg = new LinearGradientBrush(new Rectangle(0, 0, W, H), Color.FromArgb(90, 10, 10), Color.FromArgb(10, 5, 5), 90f)) g.FillRectangle(lg, 0, 0, W, H);
            }
            Text(g, "DOOM VR", new RectangleF(20, 18, W - 40, 60), 40, Color.FromArgb(220, 40, 30), true);
            Text(g, name, new RectangleF(24, 600, W - 48, 250), 76, Color.White, true);
            bmp.Save(outPath, ImageFormat.Png);
        }
    }

    public static void MakeSplash(Bitmap[] titles, string heading, string outPath) {
        const int W = 1920, H = 1080;
        using (var bmp = new Bitmap(W, H)) using (var g = Graphics.FromImage(bmp)) {
            g.SmoothingMode = SmoothingMode.AntiAlias;
            g.TextRenderingHint = TextRenderingHint.AntiAliasGridFit;
            g.Clear(Color.Black);
            // A collage of the player's own title screens, 4 x 3.
            int n = 0;
            g.InterpolationMode = InterpolationMode.HighQualityBicubic;
            for (int r = 0; r < 3; r++) for (int c = 0; c < 4; c++) {
                if (titles.Length == 0) break;
                var t = titles[n++ % titles.Length];
                g.SetClip(new Rectangle(c * 480, r * 360, 480, 360));
                Cover(g, t, new RectangleF(c * 480, r * 360, 480, 360));
                g.ResetClip();
            }
            using (var shade = new SolidBrush(Color.FromArgb(175, 0, 0, 0))) g.FillRectangle(shade, 0, 0, W, H);
            Text(g, heading, new RectangleF(100, 330, W - 200, 260), 230, Color.FromArgb(225, 35, 25), true);
            Text(g, "A GAME OR DIE PCVR PORT", new RectangleF(100, 600, W - 200, 90), 64, Color.White, true);
            Text(g, "Powered by UZDoom  -  your own games, in VR", new RectangleF(100, 960, W - 200, 60), 32, Color.FromArgb(200, 200, 200), false);
            bmp.Save(outPath, ImageFormat.Png);
        }
    }
}
}
"@
}
