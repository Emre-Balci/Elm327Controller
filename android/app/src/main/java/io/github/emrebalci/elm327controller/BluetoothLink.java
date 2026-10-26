package io.github.emrebalci.elm327controller;

import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothSocket;
import android.os.ParcelFileDescriptor;
import android.system.ErrnoException;
import android.system.Os;
import android.system.OsConstants;

import java.io.Closeable;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.UUID;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * A Bluetooth adapter linked to the native side (gui/android_bridge.hpp; the bridged link of
 * lib/include/elm327/controller/bridged_link.hpp): connects to it with the serial port profile
 * (RFCOMM), answers one status byte on the link, and then copies the bytes between the two in both
 * directions, until either side closes. Closing one closes the other.
 */
final class BluetoothLink extends Thread {
    /** The serial port profile. */
    private static final UUID SERIAL_PORT = UUID.fromString("00001101-0000-1000-8000-00805F9B34FB");

    // The status byte: elm327::LinkAnswer (lib/include/elm327/controller/bridged_link.hpp).
    private static final int CONNECTED = 0;
    private static final int NOT_FOUND = 1;
    private static final int NO_PERMISSION = 2;
    private static final int OFF = 3;
    private static final int REFUSED = 4;

    private static final int BUFFER_SIZE = 1024;

    private final BluetoothAdapter adapter;
    private final String address;
    private final ParcelFileDescriptor link;
    private final AtomicBoolean closed = new AtomicBoolean();
    private volatile BluetoothSocket socket;

    BluetoothLink(BluetoothAdapter adapter, String address, ParcelFileDescriptor link) {
        super("elm327-bluetooth");
        this.adapter = adapter;
        this.address = address;
        this.link = link;
    }

    @Override
    public void run() {
        int status = connect();
        try {
            new FileOutputStream(link.getFileDescriptor()).write(status);
        } catch (IOException gone) {
            status = REFUSED;  // The native side gave up waiting.
        }
        if (status == CONNECTED) {
            try {
                final InputStream fromAdapter = socket.getInputStream();
                final OutputStream toAdapter = socket.getOutputStream();
                final Thread upstream = new Thread(
                        () -> copy(new FileInputStream(link.getFileDescriptor()), toAdapter), "elm327-bluetooth-out");
                upstream.start();
                copy(fromAdapter, new FileOutputStream(link.getFileDescriptor()));
            } catch (IOException broken) {
                // Closed below.
            }
        }
        close();
    }

    private int connect() {
        if (adapter == null) {
            return NOT_FOUND;
        }
        try {
            if (!adapter.isEnabled()) {
                return OFF;
            }
            socket = open(adapter.getRemoteDevice(address));
            return CONNECTED;
        } catch (SecurityException refused) {
            return NO_PERMISSION;
        } catch (IllegalArgumentException invalid) {
            return NOT_FOUND;
        } catch (IOException unreachable) {
            return REFUSED;
        }
    }

    /** Clones of the ELM327 differ: most take a secure connection, some only an insecure one. */
    private static BluetoothSocket open(BluetoothDevice device) throws IOException {
        final BluetoothSocket secure = device.createRfcommSocketToServiceRecord(SERIAL_PORT);
        try {
            secure.connect();
            return secure;
        } catch (IOException refused) {
            closeQuietly(secure);
        }
        final BluetoothSocket insecure = device.createInsecureRfcommSocketToServiceRecord(SERIAL_PORT);
        try {
            insecure.connect();
            return insecure;
        } catch (IOException refused) {
            closeQuietly(insecure);
            throw refused;
        }
    }

    /** Copies {@code from} to {@code to} until either ends, then closes the link. */
    private void copy(InputStream from, OutputStream to) {
        final byte[] buffer = new byte[BUFFER_SIZE];
        try {
            for (int count = from.read(buffer); count > 0; count = from.read(buffer)) {
                to.write(buffer, 0, count);
                to.flush();
            }
        } catch (IOException ended) {
            // Closed below.
        }
        close();
    }

    /** Ends both sides once: the Bluetooth connection, and the native side's link. */
    private void close() {
        if (!closed.compareAndSet(false, true)) {
            return;
        }
        closeQuietly(socket);
        try {
            // Wakes up a read of the link on the other thread, which closing alone would not.
            Os.shutdown(link.getFileDescriptor(), OsConstants.SHUT_RDWR);
        } catch (ErrnoException alreadyClosed) {
            // The native side closed its end first.
        }
        closeQuietly(link);
    }

    private static void closeQuietly(Closeable closeable) {
        if (closeable == null) {
            return;
        }
        try {
            closeable.close();
        } catch (IOException ignored) {
            // Nothing to do about it.
        }
    }
}
