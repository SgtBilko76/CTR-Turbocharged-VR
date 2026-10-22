package com.bilko.ctrvr;

import android.content.Intent;
import android.content.res.AssetManager;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.provider.Settings;
import android.util.Log;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;

import org.libsdl.app.SDLActivity;

/**
 * CTR: Turbocharged VR. SDL3 runs the game on its own thread; the native
 * Quest layer (platform/quest/ctr_quest.c) shows it in an OpenXR session.
 * Game data lives in /sdcard/CTR (needs all-files access): the player copies
 * the disc image to /sdcard/CTR/assets/ctr-u.bin.
 */
public class CTRActivity extends SDLActivity {
    private static final String TAG = "CTRVR";
    private static final String DATA_DIR = "/sdcard/CTR";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        if (Build.VERSION.SDK_INT >= 30 && !Environment.isExternalStorageManager()) {
            Log.w(TAG, "requesting all-files access for " + DATA_DIR);
            try {
                Intent intent = new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                        Uri.parse("package:" + getPackageName()));
                startActivity(intent);
            } catch (Exception e) {
                Log.e(TAG, "cannot open the storage permission screen", e);
            }
        } else {
            copyBundledAssets();
        }
        super.onCreate(savedInstanceState);
    }

    /** APK assets/ctr/** -> /sdcard/CTR/assets/** (fonts, pad picture). */
    private void copyBundledAssets() {
        try {
            copyAssetTree(getAssets(), "ctr", new File(DATA_DIR, "assets"));
        } catch (Exception e) {
            Log.e(TAG, "copying bundled assets failed", e);
        }
    }

    private static void copyAssetTree(AssetManager assets, String path, File target) throws Exception {
        String[] children = assets.list(path);
        if (children != null && children.length > 0) {
            if (!target.isDirectory() && !target.mkdirs()) {
                Log.w(TAG, "cannot create " + target);
            }
            for (String child : children) {
                copyAssetTree(assets, path + "/" + child, new File(target, child));
            }
            return;
        }
        try (InputStream in = assets.open(path)) {
            if (target.isFile() && target.length() == in.available()) {
                return;
            }
            try (OutputStream out = new FileOutputStream(target)) {
                byte[] buffer = new byte[65536];
                int got;
                while ((got = in.read(buffer)) > 0) {
                    out.write(buffer, 0, got);
                }
            }
        }
    }

    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL3", "main" };
    }

    @Override
    protected void onDestroy() {
        // SDL joins its native thread without a timeout; make sure the
        // process (and its audio) really ends.
        Thread watchdog = new Thread(() -> {
            try { Thread.sleep(3000); } catch (InterruptedException ignored) { }
            android.os.Process.killProcess(android.os.Process.myPid());
        });
        watchdog.setDaemon(true);
        watchdog.start();
        super.onDestroy();
        android.os.Process.killProcess(android.os.Process.myPid());
    }
}
