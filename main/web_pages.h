// Setup-hotspot portal page (the station-mode web app is main/web/app.html,
// embedded at build time).
#pragma once

// Setup-hotspot page, styled like the WiFiManager portal (custom head from
// WiFiManagerHelpers.h). %SSID_OPTIONS% is replaced with scanned networks.
static const char PORTAL_HTML[] = R"(
<html>
    <head>
        <meta name="viewport" content="width=device-width, initial-scale=1">
        <title>AeroScope - Setup WiFi</title>
        <style>
            body{background:#111;color:#00ff00;font-family:monospace;margin:0;padding:16px;}
            h1{font-size:1.3em;}
            label{display:block;margin-top:14px;}
            input,select{width:100%;max-width:420px;box-sizing:border-box;background:#111;color:#00ff00;
                border:1px solid #00ff00;padding:8px;font-family:monospace;font-size:1em;}
            button{margin-top:18px;background:#00ff00;color:#111;border:0;padding:10px 18px;
                font-family:monospace;font-size:1em;cursor:pointer;}
            .hint{opacity:.7;font-size:.9em;margin-top:6px;}
        </style>
    </head>
    <body>
        <h1>AeroScope - Setup WiFi</h1>
        <form action="/wifisave" method="POST">
            <label>Network
                <select onchange="document.getElementById('s').value=this.value">
                    <option value="">-- choose a scanned network --</option>
                    %SSID_OPTIONS%
                </select>
            </label>
            <label>SSID<input id="s" name="s" maxlength="32" autocomplete="off" required></label>
            <label>Password<input name="p" type="password" maxlength="64"></label>
            <div class="hint">2.4 GHz networks only. The device restarts after saving.</div>
            <button type="submit">Save</button>
        </form>
    </body>
</html>
)";
