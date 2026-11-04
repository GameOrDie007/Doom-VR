// Doom VR on Quest and Steam Frame (standalone): the game, in the headset.
// SDL2's activity runs the engine; its main() is UZDoom's own.
//
// Where things are, under Android/data/net.gameordie.doomvr/files/game - the
// PC's VR\game folder, laid out the same way, so every path the engine keeps
// relative to its program (launchers, My WADs, boxart, the hands) works as it
// does there:
//   the engine's own files (uzdoom.pk3, game_support.pk3, lights, brightmaps,
//   soundfonts/, fm_banks/), unpacked from the APK's assets at first start and
//   again after an update (engine-version.txt says which build);
//   the player's games and settings, copied in by the PC's Setup: wads/,
//   mods/, launchers/, doomvr.ini, launch.txt.
// game/ is $HOME, the folder the engine runs in, and its program folder.
package net.gameordie.doomvr;

import android.content.Context;
import android.content.res.AssetManager;
import android.os.Bundle;
import android.system.Os;
import android.util.Log;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.Arrays;
import org.libsdl.app.SDLActivity;

public class DoomVRActivity extends SDLActivity {
    static final String TAG = "DoomVR";
    // The folders the game writes into. A folder or file the PC's Setup copies
    // in over adb belongs to adb: the game can read it but not rewrite it, so
    // the app makes these folders itself.
    static final String[] WRITABLE = { "game", "game/wads", "game/mods", "game/My WADs", "game/logs", "game/gametome", "game/screenshots" };

    // Where the app's files live: Android/data/<package>/files.
    static File dataDir(Context c) {
        File d = c.getExternalFilesDir(null);
        return d != null ? d : c.getFilesDir();
    }

    static File gameDir(Context c) { return new File(dataDir(c), "game"); }
    static File engineDir(Context c) { return gameDir(c); }

    // Make the writable folders, and take in every "name.restore" that Setup
    // copied over adb (settings kept across a reinstall) as the app's own
    // "name", which the game can then rewrite. Returns how many of the
    // folders can be written.
    static int prepare(Context c) {
        File base = dataDir(c);
        int writable = 0;
        for (String sub : WRITABLE) {
            File d = new File(base, sub);
            if (!d.isDirectory() && !d.mkdirs())
                Log.w(TAG, "cannot make " + d);
            takeIn(d);
            File probe = new File(d, ".write-test");
            try {
                new FileOutputStream(probe).close();
                if (probe.delete())
                    writable++;
            } catch (Exception e) {
                Log.w(TAG, "cannot write in " + d + ": " + e);
            }
        }
        return writable;
    }

    static void takeIn(File dir) {
        File[] files = dir.listFiles();
        if (files == null)
            return;
        for (File from : files) {
            String n = from.getName();
            if (!from.isFile() || !n.endsWith(".restore"))
                continue;
            File to = new File(dir, n.substring(0, n.length() - ".restore".length()));
            File tmp = new File(dir, to.getName() + ".new");
            try (InputStream in = new FileInputStream(from); OutputStream out = new FileOutputStream(tmp)) {
                copy(in, out);
            } catch (Exception e) {
                Log.w(TAG, "could not take in " + from + ": " + e);
                tmp.delete();
                continue;
            }
            to.delete();
            if (tmp.renameTo(to))
                from.delete();
            else
                Log.w(TAG, "could not take in " + from);
        }
    }

    static void copy(InputStream in, OutputStream out) throws java.io.IOException {
        byte[] buf = new byte[65536];
        int k;
        while ((k = in.read(buf)) > 0)
            out.write(buf, 0, k);
    }

    static String readSmall(File f) {
        try (InputStream in = new FileInputStream(f)) {
            java.io.ByteArrayOutputStream bo = new java.io.ByteArrayOutputStream();
            copy(in, bo);
            return new String(bo.toByteArray(), "UTF-8");
        } catch (Exception e) {
            return null;
        }
    }

    // The engine's own files, out of the APK into game/. assets/engine/engine-version.txt
    // names the build; when the copy on the headset carries the same name
    // nothing is copied. Returns false when something could not be written.
    static boolean unpackEngine(Context c) {
        AssetManager am = c.getAssets();
        File dst = engineDir(c);
        String want = null;
        try (InputStream in = am.open("engine/engine-version.txt")) {
            java.io.ByteArrayOutputStream bo = new java.io.ByteArrayOutputStream();
            copy(in, bo);
            want = new String(bo.toByteArray(), "UTF-8").trim();
        } catch (Exception e) {
            Log.e(TAG, "the APK carries no engine/engine-version.txt: " + e);
            return false;
        }
        String have = readSmall(new File(dst, "engine-version.txt"));
        if (have != null && have.trim().equals(want) && new File(dst, "uzdoom.pk3").isFile()) {
            Log.i(TAG, "engine " + want + " already unpacked");
            return true;
        }
        Log.i(TAG, "unpacking engine " + want + " (had " + (have == null ? "nothing" : have.trim()) + ")");
        if (!dst.isDirectory() && !dst.mkdirs()) {
            Log.e(TAG, "cannot make " + dst);
            return false;
        }
        boolean ok = true;
        try {
            String[] names = am.list("engine");
            if (names == null) names = new String[0];
            for (String n : names) {
                if (n.equals("engine-version.txt")) continue;
                String[] inner = am.list("engine/" + n);
                if (inner != null && inner.length > 0) {
                    // one level of folders (soundfonts/, fm_banks/)
                    File sub = new File(dst, n);
                    sub.mkdirs();
                    for (String m : inner)
                        ok &= unpackOne(am, "engine/" + n + "/" + m, new File(sub, m));
                } else {
                    ok &= unpackOne(am, "engine/" + n, new File(dst, n));
                }
            }
        } catch (Exception e) {
            Log.e(TAG, "unpacking the engine: " + e);
            ok = false;
        }
        if (ok) {
            try (FileOutputStream out = new FileOutputStream(new File(dst, "engine-version.txt"))) {
                out.write(want.getBytes("UTF-8"));
            } catch (Exception e) {
                Log.w(TAG, "could not write version.txt: " + e);
            }
        }
        return ok;
    }

    static boolean unpackOne(AssetManager am, String asset, File to) {
        File tmp = new File(to.getPath() + ".new");
        try (InputStream in = am.open(asset); FileOutputStream out = new FileOutputStream(tmp)) {
            copy(in, out);
        } catch (Exception e) {
            Log.e(TAG, "could not unpack " + asset + ": " + e);
            tmp.delete();
            return false;
        }
        to.delete();
        if (!tmp.renameTo(to)) {
            Log.e(TAG, "could not place " + to);
            return false;
        }
        return true;
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        int w = prepare(this);
        boolean engineOk = unpackEngine(this);
        Log.i(TAG, "data " + dataDir(this) + ", " + w + " of " + WRITABLE.length + " folders writable, engine " + (engineOk ? "ready" : "NOT READY"));
        try {
            // The engine runs in game/ (its logs, settings and screenshots go
            // there) and it is its program folder too (i_main.cpp reads
            // DOOMVR_PROGDIR on Android).
            Os.setenv("HOME", gameDir(this).getAbsolutePath(), true);
            Os.setenv("DOOMVR_PROGDIR", gameDir(this).getAbsolutePath() + "/", true);
            // The headset's maker, for the engine's log (Valve = the Steam Frame under Lepton).
            Os.setenv("DOOMVR_DEVICE", android.os.Build.MANUFACTURER + " " + android.os.Build.MODEL, true);
            // The Game Tome's folder: its layer (loaded by the OpenXR loader from this APK's
            // assets, never by the game) keeps its log and its signal file there, and the
            // books Setup copies from the PC (tome, tome-<game>, books.txt) live there.
            Os.setenv("GAMETOME_HOME", new File(gameDir(this), "gametome").getAbsolutePath(), true);
            // The real controllers on its Controls page: a PC reads them from SteamVR, a
            // headset has none, so Setup puts them beside the books (SteamVR's own layout).
            Os.setenv("GAMETOME_CONTROLLER_MODELS", new File(gameDir(this), "gametome/controllers").getAbsolutePath(), true);
        } catch (Exception e) {
            Log.w(TAG, "setenv: " + e);
        }
        super.onCreate(savedInstanceState);
    }

    @Override
    protected String[] getLibraries() {
        // In load order: SDL first, then what the engine opens by name
        // (libopenal.so is dlopen'd by the engine; ZMusic is inside libmain.so).
        return new String[] { "c++_shared", "openxr_loader", "SDL2", "openal", "main" };
    }

    // The game has ended (its main returned and SDL finished the activity): end
    // the process too. Android keeps it otherwise, with the audio stream still
    // open, and the headset shows nothing but black.
    @Override
    protected void onDestroy() {
        super.onDestroy();
        android.os.Process.killProcess(android.os.Process.myPid());
    }

    @Override
    protected String[] getArguments() {
        File game = gameDir(this);
        // What the PC launchers pass: the settings file, the headset, the log.
        // vr_mode 20 is the headset; vid_preferbackend 2 the OpenGL ES renderer.
        ArrayList<String> args = new ArrayList<>(Arrays.asList(
            "-config", new File(game, "doomvr.ini").getAbsolutePath(),
            "+vid_preferbackend", "2", "+vid_fullscreen", "1", "+vr_mode", "20", "+logfile", "doomvr", "+save_dir", "saves"));
        // The game to start and its files: launch.txt beside the settings, one
        // argument a line, written by the PC's Setup (the picker's choice goes
        // there too). Without it the engine looks for a game itself.
        File launch = new File(game, "launch.txt");
        String t = launch.isFile() ? readSmall(launch) : null;
        if (t != null) {
            for (String line : t.split("\\r?\\n")) {
                line = line.trim();
                if (!line.isEmpty() && !line.startsWith("#"))
                    args.add(line);
            }
            Log.i(TAG, "launch.txt: " + t.replace('\n', ' '));
        }
        // Desk tests only: words in desk-args.txt are added to the command
        // line (a player never has the file).
        File desk = new File(game, "desk-args.txt");
        if (desk.isFile()) {
            String d = readSmall(desk);
            if (d != null && !d.trim().isEmpty()) {
                args.addAll(Arrays.asList(d.trim().split("\\s+")));
                Log.i(TAG, "desk-args.txt: " + d.trim());
            }
        }
        return args.toArray(new String[0]);
    }
}
