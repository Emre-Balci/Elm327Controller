package io.github.emrebalci.elm327controller;

import android.Manifest;
import android.app.NativeActivity;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothManager;
import android.content.pm.PackageManager;
import android.graphics.Insets;
import android.os.Build;
import android.os.Bundle;
import android.os.ParcelFileDescriptor;
import android.view.DisplayCutout;
import android.view.KeyEvent;
import android.view.View;
import android.view.WindowInsets;
import android.view.inputmethod.InputMethodManager;

import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * The app's activity: a NativeActivity whose window libelm327gui.so draws (gui/main_android.cpp).
 * It gives the native side what only Java reaches (gui/android_bridge.hpp): the paired Bluetooth
 * devices and links to them, the on-screen keyboard, and the insets of the window.
 *
 * <p>The native side calls the public methods below through JNI, by their names and signatures.
 */
public final class MainActivity extends NativeActivity {
    // What bluetoothState() answers; in step with gui/android_bridge.cpp.
    private static final int STATE_READY = 0;
    private static final int STATE_NO_BLUETOOTH = 1;
    private static final int STATE_OFF = 2;
    private static final int STATE_NO_PERMISSION = 3;

    private static final int PERMISSION_REQUEST = 1;

    static {
        // NativeActivity loads the library too, but only this makes its JNI functions known to Java
        // (nativeSetInsets()). It is the same library either way.
        System.loadLibrary("elm327gui");
    }

    /** Characters typed on the on-screen keyboard, until the native side takes them. */
    private final LinkedBlockingQueue<Integer> characters = new LinkedBlockingQueue<>();

    /** The permission for nearby devices was asked for and has not been answered yet. */
    private final AtomicBoolean asking = new AtomicBoolean();

    /** Tells the native side what the system bars, a display cutout and the keyboard cover. */
    private static native void nativeSetInsets(int left, int top, int right, int bottom);

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        // The window reaches under the system bars (as Android 15 and later do anyway); the native
        // side keeps its content clear of them.
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            getWindow().setDecorFitsSystemWindows(false);
        } else {
            getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                    | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION);
        }
        getWindow().getDecorView().setOnApplyWindowInsetsListener((view, insets) -> {
            reportInsets(insets);
            return view.onApplyWindowInsets(insets);
        });
    }

    private static void reportInsets(WindowInsets insets) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            final Insets covered = insets.getInsets(WindowInsets.Type.systemBars()
                    | WindowInsets.Type.displayCutout() | WindowInsets.Type.ime());
            nativeSetInsets(covered.left, covered.top, covered.right, covered.bottom);
        } else {
            int left = insets.getSystemWindowInsetLeft();
            int top = insets.getSystemWindowInsetTop();
            int right = insets.getSystemWindowInsetRight();
            int bottom = insets.getSystemWindowInsetBottom();
            final DisplayCutout cutout = insets.getDisplayCutout();
            if (cutout != null) {
                left = Math.max(left, cutout.getSafeInsetLeft());
                top = Math.max(top, cutout.getSafeInsetTop());
                right = Math.max(right, cutout.getSafeInsetRight());
                bottom = Math.max(bottom, cutout.getSafeInsetBottom());
            }
            nativeSetInsets(left, top, right, bottom);
        }
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        // The keys themselves (Enter, Backspace, the arrows) reach the native side as key events;
        // the characters they type only come from here.
        if (event.getAction() == KeyEvent.ACTION_DOWN) {
            final int character = event.getUnicodeChar(event.getMetaState());
            if (character > 0) {
                characters.offer(character);
            }
        } else if ((event.getAction() == KeyEvent.ACTION_MULTIPLE) && (event.getCharacters() != null)) {
            event.getCharacters().codePoints().forEach(characters::offer);  // Text from the keyboard.
        }
        return super.dispatchKeyEvent(event);
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (requestCode == PERMISSION_REQUEST) {
            asking.set(false);  // Ask again next time if it was refused; Android stops after twice.
        }
    }

    /** @return the next character typed on the on-screen keyboard; 0 when there is none. */
    public int nextCharacter() {
        final Integer character = characters.poll();
        return (character != null) ? character : 0;
    }

    /** Shows the on-screen keyboard, or hides it. */
    public void showKeyboard(boolean show) {
        runOnUiThread(() -> {
            final InputMethodManager keyboard = getSystemService(InputMethodManager.class);
            final View view = getWindow().getDecorView();
            if (keyboard == null) {
                return;
            }
            if (show) {
                keyboard.showSoftInput(view, 0);
            } else {
                keyboard.hideSoftInputFromWindow(view.getWindowToken(), 0);
            }
        });
    }

    /**
     * @return whether the paired devices can be listed: STATE_READY, STATE_NO_BLUETOOTH, STATE_OFF,
     *     or STATE_NO_PERMISSION, after asking the person for it.
     */
    public int bluetoothState() {
        final BluetoothAdapter adapter = bluetooth();
        if (adapter == null) {
            return STATE_NO_BLUETOOTH;
        }
        if (!mayConnect()) {
            if (asking.compareAndSet(false, true)) {
                runOnUiThread(() -> requestPermissions(
                        new String[] {Manifest.permission.BLUETOOTH_CONNECT}, PERMISSION_REQUEST));
            }
            return STATE_NO_PERMISSION;
        }
        try {
            return adapter.isEnabled() ? STATE_READY : STATE_OFF;
        } catch (SecurityException refused) {
            return STATE_NO_PERMISSION;
        }
    }

    /** @return the paired devices: the name and the address of each in turn. */
    public String[] pairedDevices() {
        final List<String> list = new ArrayList<>();
        final BluetoothAdapter adapter = bluetooth();
        try {
            if ((adapter != null) && mayConnect()) {
                for (BluetoothDevice device : adapter.getBondedDevices()) {
                    final String name = device.getName();
                    list.add((name != null) ? name : "");
                    list.add(device.getAddress());
                }
            }
        } catch (SecurityException refused) {
            list.clear();
        }
        return list.toArray(new String[0]);
    }

    /**
     * Connects to the Bluetooth adapter at {@code address} and links it to the native side through
     * {@code descriptor}, one end of a socket pair, which this activity owns from now on. Returns at
     * once; see BluetoothLink.
     */
    public void connectBluetooth(String address, int descriptor) {
        final ParcelFileDescriptor link = ParcelFileDescriptor.adoptFd(descriptor);
        new BluetoothLink(bluetooth(), address, link).start();
    }

    private BluetoothAdapter bluetooth() {
        final BluetoothManager manager = getSystemService(BluetoothManager.class);
        return (manager != null) ? manager.getAdapter() : null;
    }

    /** @return true if the app may connect to nearby devices (asked for from Android 12 on). */
    private boolean mayConnect() {
        return (Build.VERSION.SDK_INT < Build.VERSION_CODES.S)
                || (checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) == PackageManager.PERMISSION_GRANTED);
    }
}
