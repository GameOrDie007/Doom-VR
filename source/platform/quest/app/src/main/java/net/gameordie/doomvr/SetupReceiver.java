// Doom VR on Quest and Steam Frame (standalone): a helper for the PC's Setup.
// Setup copies the player's games and settings to the headset over adb, and a
// folder adb makes is adb's: the game may read it but not write into it. So
// Setup asks the app, with an explicit broadcast, to make the game's folders
// itself, then copies into them. The app also takes in every "name.restore"
// Setup put there (settings kept across a reinstall) and proves it can write
// in each folder; it answers with how many folders are writable. No window.
package net.gameordie.doomvr;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.util.Log;

public class SetupReceiver extends BroadcastReceiver {
    @Override
    public void onReceive(Context context, Intent intent) {
        int writable = DoomVRActivity.prepare(context);
        Log.i(DoomVRActivity.TAG, "Setup: the game's folders: " + writable + " of " + DoomVRActivity.WRITABLE.length + " writable");
        if (isOrderedBroadcast())
            setResultCode(writable == DoomVRActivity.WRITABLE.length ? 1 : 0);
    }
}
