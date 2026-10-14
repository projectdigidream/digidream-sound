package com.digidreamsound.sfx;

import android.annotation.SuppressLint;
import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.provider.DocumentsContract;
import android.provider.OpenableColumns;
import android.util.Base64;
import android.webkit.JavascriptInterface;
import android.webkit.ValueCallback;
import android.webkit.WebChromeClient;
import android.webkit.WebResourceRequest;
import android.webkit.WebResourceResponse;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.HashMap;
import java.util.Map;

/**
 * The whole Android app: a WebView with the editor in it.
 *
 * <p>THE SAME PAGE THE BROWSER LOADS. scripts/package_android.sh stages exactly
 * what scripts/package_web.sh produces -- which is also what the Electron shell
 * ships -- so browser, desktop and phone run byte-identical tool code. The one
 * addition is {@code android-shell.js}, injected into the head of sfx.html as it
 * is served. It defines {@code window.digidream}, the same bridge the Electron
 * preload defines, so the vendored {@code tool/lib/desktop.js} takes its native
 * path and File > Open / Save / Bake / Export WAV become the system's own
 * document pickers. The page never learns it is on a phone.
 *
 * <p>WHY AN https:// ORIGIN. AudioWorklet, module Workers and WebAssembly are
 * restricted to a secure context, and file:// is not one. So every request is
 * answered from the APK's assets by {@link #serve} on {@code
 * appassets.androidplatform.net}, a reserved name that cannot resolve -- the
 * same name AndroidX's WebViewAssetLoader uses, for the same reason: a request
 * this class fails to intercept fails closed.
 *
 * <p>NO PERMISSIONS, NOT EVEN INTERNET. Unlike the game's shell, nothing here
 * opens a socket, so the manifest asks for nothing, and every request that is
 * not our own origin is answered with a 404 rather than fetched. Files move
 * only through the Storage Access Framework, which needs no permission because
 * the user picks each one.
 *
 * <p>FAILURES ARE ON THE SCREEN. The phone this runs on has no adb and no
 * logcat, so a missing page is answered with a page that says what was asked
 * and which build this is, and android-shell.js turns any uncaught error into a
 * banner. A screenshot is the bug report.
 */
public final class MainActivity extends Activity {
  private static final String ORIGIN = "https://appassets.androidplatform.net";
  private static final String HOST = "appassets.androidplatform.net";
  private static final String START = ORIGIN + "/tool/sfx.html";

  private static final int REQ_OPEN = 1;
  private static final int REQ_SAVE = 2;
  private static final int REQ_SAVE_MANY = 3;
  private static final int REQ_CHOOSER = 4;

  private WebView web;
  /** What the build wrote to build.txt, for the missing-page report. */
  private String buildInfo = "";
  /** The page's own dirty flag, from digidream.setDocument(). Guards Back. */
  private volatile boolean dirty;

  // One outstanding picker at a time: Android shows one, and the page awaits it.
  private String pendingId;
  private byte[] pendingBytes;
  private String pendingName;
  private JSONArray pendingMany;
  private ValueCallback<Uri[]> pendingChooser;

  @Override
  @SuppressLint({"SetJavaScriptEnabled", "AddJavascriptInterface"})
  protected void onCreate(Bundle state) {
    super.onCreate(state);
    buildInfo = new String(readAsset("build.txt", new byte[0]), StandardCharsets.UTF_8).trim();

    // chrome://inspect works against the emulator; the phone has the banner.
    WebView.setWebContentsDebuggingEnabled(true);

    web = new WebView(this);
    WebSettings s = web.getSettings();
    s.setJavaScriptEnabled(true);
    s.setDomStorageEnabled(true);        // the tool's UI state (tab, selection)
    s.setDatabaseEnabled(true);          // IndexedDB, for the thumbnail cache
    // The Electron shell clicks through the audio gate itself (app.js: on a
    // desktop shell there is no autoplay policy to satisfy). The same has to
    // be true here, or the auto-start makes an AudioContext that never runs.
    s.setMediaPlaybackRequiresUserGesture(false);
    s.setAllowFileAccess(false);
    s.setAllowContentAccess(false);
    s.setSupportZoom(false);
    s.setBuiltInZoomControls(false);
    web.setBackgroundColor(0xff14161a);

    web.setWebViewClient(new WebViewClient() {
      @Override
      public WebResourceResponse shouldInterceptRequest(WebView v, WebResourceRequest req) {
        WebResourceResponse res = serve(req.getUrl());
        if (req.isForMainFrame() && res.getStatusCode() == 404) {
          return html(missingPage(req.getUrl().toString()));
        }
        return res;
      }

      @Override
      public boolean shouldOverrideUrlLoading(WebView v, WebResourceRequest req) {
        // A link out of the app (a doc link, say) is not followed: there is
        // nowhere for it to go, and following it would be the one way this
        // app ever touched the network.
        return !HOST.equals(req.getUrl().getHost());
      }
    });

    web.setWebChromeClient(new WebChromeClient() {
      // The Fit tab's <input type=file accept="audio/*">. Without this a
      // WebView does nothing at all when it is tapped.
      @Override
      public boolean onShowFileChooser(WebView v, ValueCallback<Uri[]> cb, FileChooserParams p) {
        if (pendingChooser != null) pendingChooser.onReceiveValue(null);
        pendingChooser = cb;
        try {
          startActivityForResult(p.createIntent(), REQ_CHOOSER);
        } catch (Exception e) {
          pendingChooser = null;
          cb.onReceiveValue(null);
          return false;
        }
        return true;
      }
      // window.confirm/alert: the default WebChromeClient shows the system's
      // own dialogs, which is what confirmDiscard() wants.
    });

    web.addJavascriptInterface(new Bridge(), "dsndNative");
    setContentView(web);
    if (state != null) web.restoreState(state);
    else web.loadUrl(START);
  }

  @Override
  protected void onSaveInstanceState(Bundle out) {
    super.onSaveInstanceState(out);
    web.saveState(out);
  }

  @Override
  @SuppressWarnings("deprecation")
  public void onBackPressed() {
    // Back would destroy the only copy of an unsaved bank. The page tells us
    // whether it is dirty through setDocument(), exactly as it tells Electron
    // to draw the edited dot.
    if (!dirty) { super.onBackPressed(); return; }
    new AlertDialog.Builder(this)
        .setMessage("This bank has unsaved changes. Leave anyway?")
        .setPositiveButton("Leave", (d, w) -> finish())
        .setNegativeButton("Stay", null)
        .show();
  }

  // ---------------------------------------------------------------------------
  // Serving the assets.
  // ---------------------------------------------------------------------------

  private WebResourceResponse serve(Uri url) {
    if (!HOST.equals(url.getHost())) return notFound();
    String path = url.getPath();
    if (path == null || path.equals("/") || path.isEmpty()) path = "/tool/sfx.html";
    if (path.contains("..")) return notFound();
    byte[] data = readAsset("web" + path, null);
    if (data == null) return notFound();

    if (path.equals("/tool/sfx.html")) {
      // A CLASSIC script, inserted first in <head>, so window.digidream exists
      // before the page's module graph evaluates desktop.js -- which reads
      // globalThis.digidream once, at import.
      String page = new String(data, StandardCharsets.UTF_8);
      int at = page.indexOf("<head>");
      if (at >= 0) {
        at += "<head>".length();
        page = page.substring(0, at) + "\n<script src=\"/android-shell.js\"></script>"
             + page.substring(at);
      }
      data = page.getBytes(StandardCharsets.UTF_8);
    }
    WebResourceResponse r = new WebResourceResponse(mimeOf(path), null, new ByteArrayInputStream(data));
    Map<String, String> h = new HashMap<>();
    // The assets are in the APK; a stale cache across an upgrade is the only
    // thing caching could buy here.
    h.put("Cache-Control", "no-store");
    r.setResponseHeaders(h);
    return r;
  }

  private static String mimeOf(String path) {
    String p = path.toLowerCase();
    if (p.endsWith(".html")) return "text/html";
    // .mjs MUST be text/javascript: a module script with any other type is
    // refused, and the engine is an .mjs.
    if (p.endsWith(".js") || p.endsWith(".mjs")) return "text/javascript";
    if (p.endsWith(".css")) return "text/css";
    if (p.endsWith(".json") || p.endsWith(".sfxproj")) return "application/json";
    if (p.endsWith(".wasm")) return "application/wasm";
    if (p.endsWith(".svg")) return "image/svg+xml";
    if (p.endsWith(".png")) return "image/png";
    if (p.endsWith(".wav")) return "audio/wav";
    if (p.endsWith(".txt") || p.endsWith(".md")) return "text/plain";
    return "application/octet-stream";
  }

  private byte[] readAsset(String name, byte[] missing) {
    try (InputStream in = getAssets().open(name)) {
      return readAll(in);
    } catch (IOException e) {
      return missing;
    }
  }

  private static byte[] readAll(InputStream in) throws IOException {
    ByteArrayOutputStream out = new ByteArrayOutputStream();
    byte[] buf = new byte[65536];
    for (int n; (n = in.read(buf)) > 0; ) out.write(buf, 0, n);
    return out.toByteArray();
  }

  private static WebResourceResponse notFound() {
    return new WebResourceResponse("text/plain", "utf-8", 404, "Not Found",
        new HashMap<String, String>(), new ByteArrayInputStream(new byte[0]));
  }

  private static WebResourceResponse html(String s) {
    return new WebResourceResponse("text/html", "utf-8",
        new ByteArrayInputStream(s.getBytes(StandardCharsets.UTF_8)));
  }

  private String missingPage(String url) {
    StringBuilder b = new StringBuilder();
    b.append("<!doctype html><meta name=viewport content='width=device-width'>")
     .append("<body style='background:#14161a;color:#d6d9e0;font:14px/1.5 sans-serif;padding:16px'>")
     .append("<h2 style='color:#ff8a80'>DigidreamSound: page not in this APK</h2>")
     .append("<p>asked for <code>").append(esc(url)).append("</code></p>")
     .append("<p>build</p><pre>").append(esc(buildInfo.isEmpty() ? "(no build.txt)" : buildInfo))
     .append("</pre><p>assets/web/tool holds</p><pre>");
    try {
      String[] names = getAssets().list("web/tool");
      if (names != null) for (String n : names) b.append(esc(n)).append('\n');
    } catch (IOException e) {
      b.append("(cannot list: ").append(esc(e.getMessage())).append(')');
    }
    return b.append("</pre>").toString();
  }

  private static String esc(String s) {
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;");
  }

  // ---------------------------------------------------------------------------
  // The bridge. Every call carries an id; the answer comes back through
  // window.__dsndReply(id, value), which android-shell.js turns into the
  // resolution of the promise the page is awaiting.
  // ---------------------------------------------------------------------------

  private void reply(final String id, final String json) {
    runOnUiThread(() -> web.evaluateJavascript(
        "window.__dsndReply(" + JSONObject.quote(id) + "," + json + ")", null));
  }

  private void fail(String id, Exception e) {
    reply(id, "{\"error\":" + JSONObject.quote(String.valueOf(e.getMessage())) + "}");
  }

  private final class Bridge {
    @JavascriptInterface
    public String build() { return buildInfo; }

    @JavascriptInterface
    public void setDocument(String json) {
      try { dirty = new JSONObject(json).optBoolean("dirty", false); } catch (Exception e) { /* keep */ }
    }

    /** exts: JSON array of extensions without dots. Answers [{name, path, data(base64)}]. */
    @JavascriptInterface
    public void open(final String id, final String exts, final boolean multiple) {
      runOnUiThread(() -> {
        pendingId = id;
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        // SAF filters by MIME, and .sfxproj has none a phone knows; so open
        // anything, unless every extension asked for is audio.
        i.setType(allAudio(exts) ? "audio/*" : "*/*");
        i.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, multiple);
        i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION
                 | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        start(i, REQ_OPEN, id);
      });
    }

    /**
     * Write one file. With toPath (a content:// URI a previous open or save
     * returned), overwrite it in place -- that is File > Save. Otherwise, or
     * if that document is no longer writable, ask where. Answers {name, path}
     * or null when cancelled.
     */
    @JavascriptInterface
    public void save(final String id, final String name, final String mime, final String b64,
                     final String toPath) {
      final byte[] data = Base64.decode(b64, Base64.DEFAULT);
      if (toPath != null && toPath.startsWith("content://")) {
        try {
          Uri u = Uri.parse(toPath);
          write(u, data);
          reply(id, fileJson(displayName(u, name), u));
          return;
        } catch (Exception e) {
          // Moved, deleted, or its permission lapsed: fall through to a picker.
        }
      }
      runOnUiThread(() -> {
        pendingId = id;
        pendingBytes = data;
        pendingName = name;
        Intent i = new Intent(Intent.ACTION_CREATE_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType(mime == null || mime.isEmpty() ? "application/octet-stream" : mime);
        i.putExtra(Intent.EXTRA_TITLE, name);
        start(i, REQ_SAVE, id);
      });
    }

    /** files: JSON [{name, data(base64)}] into one folder the user picks. Answers {dir, count}. */
    @JavascriptInterface
    public void saveMany(final String id, final String files) {
      runOnUiThread(() -> {
        try {
          pendingMany = new JSONArray(files);
        } catch (Exception e) {
          fail(id, e);
          return;
        }
        pendingId = id;
        start(new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), REQ_SAVE_MANY, id);
      });
    }

    @JavascriptInterface
    public void confirm(final String id, final String message, final String detail, final String ok) {
      runOnUiThread(() -> {
        final boolean[] answered = {false};
        new AlertDialog.Builder(MainActivity.this)
            .setTitle(message)
            .setMessage(detail == null || detail.isEmpty() ? null : detail)
            .setPositiveButton(ok == null || ok.isEmpty() ? "OK" : ok,
                (d, w) -> { answered[0] = true; reply(id, "true"); })
            .setNegativeButton("Cancel", (d, w) -> { answered[0] = true; reply(id, "false"); })
            .setOnDismissListener(d -> { if (!answered[0]) reply(id, "false"); })
            .show();
      });
    }
  }

  private static boolean allAudio(String exts) {
    try {
      JSONArray a = new JSONArray(exts);
      if (a.length() == 0) return false;
      for (int i = 0; i < a.length(); i++) {
        String e = a.getString(i).toLowerCase();
        if (!(e.equals("wav") || e.equals("mp3") || e.equals("ogg") || e.equals("flac"))) return false;
      }
      return true;
    } catch (Exception e) {
      return false;
    }
  }

  private void start(Intent i, int req, String id) {
    try {
      startActivityForResult(i, req);
    } catch (Exception e) {
      // No document UI on this device: say so in the page, not in a log.
      pendingId = null;
      fail(id, new Exception("no file picker on this device: " + e.getMessage()));
    }
  }

  @Override
  protected void onActivityResult(int req, int result, Intent data) {
    if (req == REQ_CHOOSER) {
      ValueCallback<Uri[]> cb = pendingChooser;
      pendingChooser = null;
      if (cb != null) cb.onReceiveValue(WebChromeClient.FileChooserParams.parseResult(result, data));
      return;
    }
    final String id = pendingId;
    pendingId = null;
    if (id == null) return;
    if (result != RESULT_OK || data == null) {
      pendingBytes = null;
      pendingMany = null;
      reply(id, req == REQ_OPEN ? "[]" : "null");
      return;
    }
    // Off the UI thread: a WAV export is megabytes, and a slow provider (a
    // cloud drive) can take seconds to accept it.
    final byte[] bytes = pendingBytes;
    final String name = pendingName;
    final JSONArray many = pendingMany;
    pendingBytes = null;
    pendingMany = null;
    new Thread(() -> {
      try {
        if (req == REQ_OPEN) reply(id, opened(data));
        else if (req == REQ_SAVE) {
          Uri u = data.getData();
          keep(u);
          write(u, bytes);
          reply(id, fileJson(displayName(u, name), u));
        } else if (req == REQ_SAVE_MANY) {
          reply(id, savedMany(data.getData(), many));
        }
      } catch (Exception e) {
        fail(id, e);
      }
    }, "dsnd-io").start();
  }

  private String opened(Intent data) throws Exception {
    JSONArray out = new JSONArray();
    if (data.getClipData() != null) {
      for (int i = 0; i < data.getClipData().getItemCount(); i++)
        out.put(openedOne(data.getClipData().getItemAt(i).getUri()));
    } else if (data.getData() != null) {
      out.put(openedOne(data.getData()));
    }
    return out.toString();
  }

  private JSONObject openedOne(Uri u) throws Exception {
    keep(u);
    byte[] b;
    try (InputStream in = getContentResolver().openInputStream(u)) {
      if (in == null) throw new IOException("cannot read " + u);
      b = readAll(in);
    }
    JSONObject o = new JSONObject();
    o.put("name", displayName(u, "file"));
    o.put("path", u.toString());
    o.put("data", Base64.encodeToString(b, Base64.NO_WRAP));
    return o;
  }

  private String savedMany(Uri tree, JSONArray files) throws Exception {
    keep(tree);
    Uri dir = DocumentsContract.buildDocumentUriUsingTree(tree,
        DocumentsContract.getTreeDocumentId(tree));
    int n = 0;
    for (int i = 0; i < files.length(); i++) {
      JSONObject f = files.getJSONObject(i);
      String fname = f.getString("name");
      Uri doc = DocumentsContract.createDocument(getContentResolver(), dir,
          "application/octet-stream", fname);
      if (doc == null) throw new IOException("cannot create " + fname);
      write(doc, Base64.decode(f.getString("data"), Base64.DEFAULT));
      n++;
    }
    return "{\"dir\":" + JSONObject.quote(tree.toString()) + ",\"count\":" + n + "}";
  }

  /** Keep access across restarts, so File > Save still reaches the file tomorrow. */
  private void keep(Uri u) {
    try {
      getContentResolver().takePersistableUriPermission(u,
          Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
    } catch (Exception e) {
      // Not every provider offers persistable grants; this session's is enough.
    }
  }

  private void write(Uri u, byte[] data) throws IOException {
    // "wt": truncate. Plain "w" on some providers overwrites in place and
    // leaves the old tail behind a shorter file -- a corrupt .sfxproj.
    try (OutputStream o = getContentResolver().openOutputStream(u, "wt")) {
      if (o == null) throw new IOException("cannot write " + u);
      o.write(data);
    }
  }

  private String displayName(Uri u, String fallback) {
    try (Cursor c = getContentResolver().query(u,
        new String[] {OpenableColumns.DISPLAY_NAME}, null, null, null)) {
      if (c != null && c.moveToFirst() && !c.isNull(0)) return c.getString(0);
    } catch (Exception e) {
      // fall back
    }
    return fallback;
  }

  private static String fileJson(String name, Uri u) {
    return "{\"name\":" + JSONObject.quote(name) + ",\"path\":" + JSONObject.quote(u.toString()) + "}";
  }
}
