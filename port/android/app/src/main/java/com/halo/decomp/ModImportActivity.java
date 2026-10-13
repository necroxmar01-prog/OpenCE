package com.halo.decomp;

import android.app.Activity;
import android.content.Intent;
import android.graphics.Color;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.TypedValue;
import android.view.Gravity;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.regex.Pattern;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/**
 * opence-mods: the mods a Steam Deck's opence-mods made for Android (its
 * "Exportar para Android": opence-mods-android.zip), opened with the app
 * from a file manager or a chat app, put in the game's data folder: the
 * armors (armors/*.txt), their characters and the mods' weapons
 * (addons/*.map) and which weapons they replace (weapons/*.txt), as the
 * desktop's opence-mods/run/ has them (port/linux/game/player_armor.c,
 * addon_tags.c, weapon_mods.c read them there).
 *
 * The zip is untrusted: only those three folders, plain file names of the
 * right extension, and a bounded size are taken; the mods it brings replace
 * the ones an earlier zip brought.
 */
public class ModImportActivity extends Activity {
    /** the zip's own file, that says it is one */
    private static final String MARKER = "opence-mods-android.txt";
    private static final String[] FOLDERS = { "armors", "addons", "weapons" };
    private static final Pattern NAME = Pattern.compile("[A-Za-z0-9._ ()+-]{1,96}");
    private static final long MAXIMUM_FILE = 256L * 1024 * 1024;
    private static final long MAXIMUM_TOTAL = 1024L * 1024 * 1024;
    private static final int MAXIMUM_FILES = 512;

    private File dataRoot;
    private TextView status;
    private Button play;
    private final Handler handler = new Handler(Looper.getMainLooper());

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        dataRoot = getExternalFilesDir(null);
        buildInterface();
        Uri zip = zipOf(getIntent());
        if (zip == null || dataRoot == null) {
            done("No llegó ningún archivo de mods.", false);
            return;
        }
        status.setText("Instalando los mods...");
        new Thread(() -> importZip(zip), "opence-mods-import").start();
    }

    private static Uri zipOf(Intent intent) {
        if (intent == null)
            return null;
        if (Intent.ACTION_SEND.equals(intent.getAction())) {
            Object stream = intent.getParcelableExtra(Intent.EXTRA_STREAM);
            return stream instanceof Uri ? (Uri) stream : null;
        }
        return intent.getData();
    }

    private int dp(float value) {
        return (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, value,
            getResources().getDisplayMetrics());
    }

    private void buildInterface() {
        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);
        layout.setGravity(Gravity.CENTER);
        layout.setPadding(dp(48), dp(24), dp(48), dp(24));
        layout.setBackgroundColor(Color.rgb(12, 16, 20));

        TextView title = new TextView(this);
        title.setText("Mods de Halo");
        title.setTextColor(Color.WHITE);
        title.setTextSize(TypedValue.COMPLEX_UNIT_SP, 24);
        title.setGravity(Gravity.CENTER);
        layout.addView(title);

        status = new TextView(this);
        status.setTextColor(Color.rgb(200, 205, 210));
        status.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15);
        status.setGravity(Gravity.CENTER);
        status.setPadding(0, dp(16), 0, dp(16));
        layout.addView(status);

        play = new Button(this);
        play.setText("Jugar");
        play.setEnabled(false);
        play.setOnClickListener(v -> {
            // (the launcher asks for the disc image first if it is missing)
            Intent intent = new Intent(this, LauncherActivity.class);
            intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK);
            startActivity(intent);
            finish();
        });
        layout.addView(play, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT,
            LinearLayout.LayoutParams.WRAP_CONTENT));

        setContentView(layout);
    }

    private void done(String text, boolean playable) {
        handler.post(() -> {
            status.setText(text);
            play.setEnabled(playable);
            play.requestFocus();
        });
    }

    /** the folder and the file name of an entry the zip may bring, else null */
    private static String[] allowed(String name) {
        String[] parts = name.replace('\\', '/').split("/");
        String extension;

        if (parts.length != 2 || !NAME.matcher(parts[1]).matches() || parts[1].startsWith("."))
            return null;
        extension = parts[0].equals("addons") ? ".map" : ".txt";
        for (String folder : FOLDERS) {
            if (folder.equals(parts[0]) && parts[1].toLowerCase().endsWith(extension))
                return parts;
        }
        return null;
    }

    private void importZip(Uri zip) {
        File staging = new File(dataRoot, "opence-mods-import");
        List<String> armors = new ArrayList<>();
        int weapons = 0, addons = 0, files = 0;
        long total = 0;
        boolean marked = false;

        deleteTree(staging);
        staging.mkdirs();
        try (InputStream raw = getContentResolver().openInputStream(zip)) {
            if (raw == null)
                throw new IOException("no se pudo abrir el archivo");
            try (ZipInputStream in = new ZipInputStream(raw)) {
                byte[] buffer = new byte[65536];
                ZipEntry entry;

                while ((entry = in.getNextEntry()) != null) {
                    String[] parts;
                    File target;
                    long size = 0;
                    int read;

                    if (entry.isDirectory())
                        continue;
                    if (entry.getName().equals(MARKER)) {
                        marked = true;
                        continue;
                    }
                    parts = allowed(entry.getName());
                    if (parts == null)
                        continue;
                    if (++files > MAXIMUM_FILES)
                        throw new IOException("el archivo trae demasiados archivos");
                    new File(staging, parts[0]).mkdirs();
                    target = new File(new File(staging, parts[0]), parts[1]);
                    try (OutputStream out = new FileOutputStream(target)) {
                        while ((read = in.read(buffer)) > 0) {
                            size += read;
                            total += read;
                            if (size > MAXIMUM_FILE || total > MAXIMUM_TOTAL)
                                throw new IOException("el archivo es demasiado grande");
                            out.write(buffer, 0, read);
                        }
                    }
                    if (parts[0].equals("armors"))
                        armors.add(armorName(target, parts[1]));
                    else if (parts[0].equals("weapons"))
                        weapons++;
                    else
                        addons++;
                }
            }
            if (!marked)
                throw new IOException("no es un archivo de mods de opence-mods "
                    + "(se hace en la Steam Deck: Opciones > Exportar para Android)");
            // the mods it brings in place of the ones before
            for (String folder : FOLDERS) {
                File installed = new File(dataRoot, folder);
                File fresh = new File(staging, folder);

                deleteTree(installed);
                if (fresh.isDirectory() && !fresh.renameTo(installed))
                    throw new IOException("no se pudo guardar " + folder);
            }
            deleteTree(staging);
        } catch (Exception exception) {
            deleteTree(staging);
            done("No se pudieron instalar los mods: " + exception.getMessage(), true);
            return;
        }
        StringBuilder text = new StringBuilder("Mods instalados.\n\n");
        if (!armors.isEmpty())
            text.append("Armaduras: ").append(String.join(", ", armors)).append("\n");
        if (weapons > 0)
            text.append("Armas: ").append(weapons).append("\n");
        text.append("\nEn el juego: Settings > tu perfil > Change Color > ARMOR. "
            + "Si el juego ya estaba abierto, ciérralo y ábrelo de nuevo.");
        if (armors.isEmpty() && weapons == 0 && addons == 0)
            text = new StringBuilder("El archivo no traía mods: se quitaron los que había.");
        done(text.toString(), true);
    }

    /** an armor file's name = line, else its file name */
    private static String armorName(File file, String fallback) {
        try (BufferedReader reader = new BufferedReader(new InputStreamReader(
                new java.io.FileInputStream(file), StandardCharsets.UTF_8))) {
            String line;

            while ((line = reader.readLine()) != null) {
                String trimmed = line.trim();
                int equals = trimmed.indexOf('=');

                if (equals > 0 && trimmed.substring(0, equals).trim().equalsIgnoreCase("name"))
                    return trimmed.substring(equals + 1).trim();
            }
        } catch (IOException e) {
            // (the file name, then)
        }
        return fallback.replaceAll("\\.txt$", "");
    }

    private static void deleteTree(File file) {
        File[] children = file.listFiles();

        if (children != null) {
            for (File child : children)
                deleteTree(child);
        }
        file.delete();
    }
}
