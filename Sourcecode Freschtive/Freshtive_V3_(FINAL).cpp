#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <RTClib.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>

// ── HARDWARE ────────────────────────────────────────────────────
#define MQ135_PIN 34
#define RELAY_PIN  4

LiquidCrystal_I2C lcd(0x27, 16, 2);
RTC_DS3231        rtc;

// ── ACCESS POINT ────────────────────────────────────────────────
const char* AP_SSID = "Freschtive";
const char* AP_PASS = "Freschtive";

WebServer   server(80);
Preferences prefs;

// ── PENGATURAN (default) ─────────────────────────────────────────
int jadwalSholat[5][2] = {{4,15},{11,30},{14,50},{17,25},{18,35}};
const char* namaSholat[5] = {"Subuh","Dzuhur","Ashar","Maghrib","Isya"};

int           jumlahSemprot     = 4;
int           jarakMenit        = 10;
int           ambangBatasPolusi = 1000;
unsigned long cooldownSemprot   = 600000;
unsigned long durasiSemprot     = 500;

// ── RUNTIME ─────────────────────────────────────────────────────
String        logAktivitas = "";
unsigned long waktuSemprotAdaptifTerakhir = 0;
int           lastFiredMinute[5][10]; // [sholat][semprot] = jam*60+menit, -1=belum

// ── LOG ─────────────────────────────────────────────────────────
void tambahLog(const String& pesan) {
  DateTime now = rtc.now();
  char buf[10];
  sprintf(buf, "%02d:%02d:%02d", now.hour(), now.minute(), now.second());
  logAktivitas = String(buf) + "  " + pesan + "\n" + logAktivitas;
  int nl = 0;
  for (int i = 0; i < (int)logAktivitas.length(); i++) {
    if (logAktivitas[i] == '\n' && ++nl >= 10) { logAktivitas = logAktivitas.substring(0, i); break; }
  }
}

// ── SIMPAN / MUAT SETTING ────────────────────────────────────────
void simpanSetting() {
  prefs.begin("mac", false);
  for (int i = 0; i < 5; i++) {
    prefs.putInt(("sh"+String(i)+"j").c_str(), jadwalSholat[i][0]);
    prefs.putInt(("sh"+String(i)+"m").c_str(), jadwalSholat[i][1]);
  }
  prefs.putInt("jmlSemprot", jumlahSemprot);
  prefs.putInt("jarakMenit", jarakMenit);
  prefs.putInt("ambang",     ambangBatasPolusi);
  prefs.putULong("cooldown", cooldownSemprot);
  prefs.putULong("durasi",   durasiSemprot);
  prefs.end();
}

void muatSetting() {
  prefs.begin("mac", true);
  for (int i = 0; i < 5; i++) {
    jadwalSholat[i][0] = prefs.getInt(("sh"+String(i)+"j").c_str(), jadwalSholat[i][0]);
    jadwalSholat[i][1] = prefs.getInt(("sh"+String(i)+"m").c_str(), jadwalSholat[i][1]);
  }
  jumlahSemprot     = prefs.getInt("jmlSemprot", jumlahSemprot);
  jarakMenit        = prefs.getInt("jarakMenit", jarakMenit);
  ambangBatasPolusi = prefs.getInt("ambang",     ambangBatasPolusi);
  cooldownSemprot   = prefs.getULong("cooldown", cooldownSemprot);
  durasiSemprot     = prefs.getULong("durasi",   durasiSemprot);
  prefs.end();
}

// ── SEMPROT ─────────────────────────────────────────────────────
void semprotFragrance(unsigned long durasiMs, const String& alasan) {
  Serial.println("[SEMPROT] " + alasan + " | " + String(durasiMs) + "ms");
  tambahLog("SPRAY: " + alasan);
  lcd.setCursor(0, 1); lcd.print("STATUS: SPRAYING");
  digitalWrite(RELAY_PIN, LOW);
  delay(durasiMs);
  digitalWrite(RELAY_PIN, HIGH);
  lcd.setCursor(0, 1); lcd.print("                ");
}

// ── HTML DASHBOARD ───────────────────────────────────────────────
const char DASHBOARD_HTML[] PROGMEM = R"rawhtml(
<!DOCTYPE html>
<html lang="id">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1">
<title>Air Clean — Masjid</title>
<link href="https://fonts.googleapis.com/css2?family=DM+Mono:wght@300;400;500&family=DM+Sans:wght@300;400;500&display=swap" rel="stylesheet">
<style>
*{box-sizing:border-box;margin:0;padding:0;-webkit-tap-highlight-color:transparent}
:root{
  --ink:#1a1a18;--ink2:#5c5c58;--ink3:#9a9a94;
  --paper:#f5f4f0;--surface:#fff;--line:#e2e1db;--line2:#d0cfc8;
  --green:#1c6b40;--green-bg:#edf5f0;--green-mid:#2d9b5c;
  --amber:#8a5c00;--amber-bg:#fdf4e3;
  --red:#b83232;--red-bg:#fdf0f0;
  --r:10px;--r-sm:6px
}
@media(prefers-color-scheme:dark){:root{
  --ink:#e8e7e0;--ink2:#a0a09a;--ink3:#606058;
  --paper:#141412;--surface:#1e1e1b;--line:#2a2a26;--line2:#343430;
  --green:#3dbd78;--green-bg:#0d2318;--green-mid:#2d9b5c;
  --amber:#e09b30;--amber-bg:#201800;--red:#e06060;--red-bg:#200808;
}}
body{font-family:'DM Sans',sans-serif;background:var(--paper);color:var(--ink);font-size:15px;line-height:1.5;min-height:100vh}
.mono{font-family:'DM Mono',monospace}

/* ── WELCOME OVERLAY ── */
#welcome-overlay{position:fixed;inset:0;z-index:200;background:linear-gradient(145deg,#F5FBF7,#E8F5EC);display:flex;align-items:center;justify-content:center;transition:opacity .6s ease,visibility .6s ease}
#welcome-overlay.hidden{opacity:0;visibility:hidden;pointer-events:none}
.welcome-card{text-align:center;padding:28px 24px;animation:floatUp .8s ease forwards}
.welcome-heading{font-family:'Syne',sans-serif;font-size:46px;font-weight:800;color:#0F5132;line-height:1;letter-spacing:-1.8px;margin-bottom:18px;animation:floatUp .8s ease forwards;opacity:0}
.welcome-logo{width:92px;height:92px;margin:0 auto 18px;animation:floatUp .8s ease forwards,floating 3s ease-in-out infinite;opacity:0}
.welcome-logo svg{width:100%;height:100%;display:block}
.welcome-title{font-family:'Syne',sans-serif;font-size:34px;font-weight:800;color:#14532D;letter-spacing:-1px;animation:floatUp .8s .15s ease forwards;opacity:0}
.welcome-sub{margin-top:6px;font-size:14px;color:#64748B;letter-spacing:.8px;animation:floatUp .8s .25s ease forwards;opacity:0}
.welcome-status{margin-top:10px;font-size:12px;font-weight:700;color:#16A34A;letter-spacing:1.6px;text-transform:uppercase;animation:floatUp .8s .35s ease forwards;opacity:0}
.welcome-loader{display:flex;justify-content:center;gap:8px;margin-top:20px;animation:floatUp .8s .45s ease forwards;opacity:0}
.welcome-loader span{width:8px;height:8px;border-radius:50%;background:#3AB874;animation:pulse 1.2s infinite}
.welcome-loader span:nth-child(2){animation-delay:.2s}
.welcome-loader span:nth-child(3){animation-delay:.4s}
@keyframes floatUp{from{opacity:0;transform:translateY(18px)}to{opacity:1;transform:translateY(0)}}
@keyframes floating{0%,100%{transform:translateY(0)}50%{transform:translateY(-5px)}}
@keyframes pulse{0%,100%{transform:scale(.7);opacity:.4}50%{transform:scale(1.15);opacity:1}}
@media(max-width:480px){
  .welcome-heading{font-size:34px;margin-bottom:16px}
  .welcome-logo{width:82px;height:82px}
  .welcome-title{font-size:28px}
  .welcome-sub{font-size:13px}
  .welcome-status{font-size:11px;letter-spacing:1px}
}

/* ── HEADER ── */
header{padding:20px 20px 0;display:flex;align-items:center;justify-content:space-between}
.brand{display:flex;align-items:center;gap:10px}
.brand-logo{width:200px;max-width:100%}
.brand-logo svg{width:100%;height:auto;display:block}
.brand-name{font-size:15px;font-weight:500}
.brand-sub{font-size:11px;color:var(--ink3);margin-top:1px}
.conn-pill{display:flex;align-items:center;gap:5px;font-size:11px;color:var(--green);background:var(--green-bg);padding:5px 10px;border-radius:20px;font-family:'DM Mono',monospace}
.conn-dot{width:6px;height:6px;border-radius:50%;background:var(--green);animation:blink 2s infinite}
@keyframes blink{0%,100%{opacity:1}50%{opacity:.3}}

/* ── CLOCK & STATS ── */
.clock-section{padding:24px 20px 20px;border-bottom:1px solid var(--line)}
.time-display{font-family:'DM Mono',monospace;font-size:52px;font-weight:300;letter-spacing:-2px;line-height:1}
.date-display{font-size:12px;color:var(--ink3);margin-top:6px;font-family:'DM Mono',monospace}
.stat-row{display:grid;grid-template-columns:1fr 1fr;gap:1px;background:var(--line);border-top:1px solid var(--line);border-bottom:1px solid var(--line)}
.stat{background:var(--surface);padding:14px 16px}
.stat-label{font-size:10px;color:var(--ink3);letter-spacing:1px;text-transform:uppercase;margin-bottom:4px}
.stat-value{font-family:'DM Mono',monospace;font-size:22px;font-weight:500;line-height:1}
.stat-value.ok{color:var(--green)}.stat-value.warn{color:var(--amber)}.stat-value.bad{color:var(--red)}
.stat-unit{font-size:11px;color:var(--ink3);margin-top:2px;font-family:'DM Mono',monospace}

/* ── COOLDOWN ── */
.cooldown-card{background:var(--surface);padding:14px 16px;border-top:1px solid var(--line);border-bottom:1px solid var(--line)}
.cooldown-label{font-size:10px;color:var(--ink3);letter-spacing:1px;text-transform:uppercase;margin-bottom:4px}
.cooldown-value{font-family:'DM Mono',monospace;font-size:22px;font-weight:600;line-height:1;transition:.25s ease}
.cooldown-value.ready{color:var(--green)}.cooldown-value.wait{color:var(--amber)}

/* ── AIR QUALITY ── */
.aq-section{padding:16px 20px;border-bottom:1px solid var(--line)}
.aq-header{display:flex;justify-content:space-between;align-items:center;margin-bottom:10px}
.aq-label{font-size:11px;letter-spacing:1px;text-transform:uppercase;color:var(--ink3)}
.aq-badge{font-size:11px;font-family:'DM Mono',monospace;padding:3px 8px;border-radius:4px;font-weight:500}
.aq-badge.ok{background:var(--green-bg);color:var(--green)}
.aq-badge.warn{background:var(--amber-bg);color:var(--amber)}
.aq-badge.bad{background:var(--red-bg);color:var(--red)}
.bar-track{height:3px;background:var(--line2);border-radius:2px;overflow:hidden}
.bar-fill{height:100%;border-radius:2px;transition:width .5s ease,background .5s ease}
.bar-ticks{display:flex;justify-content:space-between;font-size:10px;color:var(--ink3);margin-top:5px;font-family:'DM Mono',monospace}

/* ── SECTIONS / FORMS ── */
section{padding:20px;border-bottom:1px solid var(--line)}
.section-title{font-size:10px;letter-spacing:1.5px;text-transform:uppercase;color:var(--ink3);margin-bottom:14px;display:flex;align-items:center;gap:6px}
.section-title::after{content:'';flex:1;height:1px;background:var(--line)}
.prayer-grid{display:grid;grid-template-columns:repeat(5,1fr);gap:8px}
.prayer-name{font-size:11px;color:var(--ink3);text-align:center;margin-bottom:4px;white-space:nowrap}
.prayer-time{width:100%;background:var(--paper);border:1px solid var(--line2);color:var(--ink);padding:8px 4px;border-radius:var(--r-sm);font-family:'DM Mono',monospace;font-size:12px;text-align:center;outline:none}
.prayer-time:focus{border-color:var(--green-mid)}
@media(max-width:768px){.prayer-grid{grid-template-columns:repeat(3,1fr)}}
@media(max-width:480px){.prayer-grid{grid-template-columns:repeat(2,1fr);gap:10px}.prayer-name{font-size:12px}.prayer-time{font-size:13px;padding:10px 6px}}
.field{margin-bottom:14px}
.field-label{font-size:11px;color:var(--ink2);margin-bottom:5px;display:block}
.field-row{display:grid;grid-template-columns:1fr 1fr;gap:10px}
input[type=number],input[type=datetime-local]{width:100%;background:var(--paper);border:1px solid var(--line2);color:var(--ink);padding:9px 12px;border-radius:var(--r-sm);font-family:'DM Mono',monospace;font-size:13px;outline:none;-webkit-appearance:none}
input[type=number]:focus,input[type=datetime-local]:focus{border-color:var(--green-mid)}
input[type=number]{-moz-appearance:textfield}
input[type=number]::-webkit-inner-spin-button{-webkit-appearance:none}

/* ── BUTTONS ── */
.btn{width:100%;padding:12px;border:none;border-radius:var(--r-sm);font-family:'DM Sans',sans-serif;font-size:13px;font-weight:500;cursor:pointer;transition:opacity .15s,transform .1s;display:flex;align-items:center;justify-content:center;gap:7px}
.btn:active{transform:scale(.98);opacity:.85}
.btn svg{width:15px;height:15px;stroke:currentColor;fill:none;stroke-width:1.8;stroke-linecap:round;stroke-linejoin:round;flex-shrink:0}
.btn-primary{background:var(--green);color:#fff;margin-bottom:8px}
.btn-secondary{background:var(--surface);border:1px solid var(--line2);color:var(--ink2)}
.btn-ghost{background:transparent;border:1px solid var(--line2);color:var(--ink2);font-size:12px;padding:9px}

/* ── MISC ── */
.info-note{background:var(--green-bg);border-left:2px solid var(--green-mid);padding:10px 12px;border-radius:0 var(--r-sm) var(--r-sm) 0;font-size:12px;color:var(--ink2);line-height:1.5;margin-bottom:14px}
.log-box{background:var(--paper);border:1px solid var(--line);border-radius:var(--r-sm);padding:12px;font-family:'DM Mono',monospace;font-size:11px;color:var(--ink3);line-height:1.8;max-height:160px;overflow-y:auto;white-space:pre-wrap;word-break:break-all}
.relay-status{display:inline-flex;align-items:center;gap:5px;font-size:11px;padding:4px 10px;border-radius:20px;font-family:'DM Mono',monospace}
.relay-status.standby{background:var(--green-bg);color:var(--green)}
.relay-status.active{background:var(--red-bg);color:var(--red)}
.relay-status .dot{width:6px;height:6px;border-radius:50%;background:currentColor}
.relay-status.active .dot{animation:blink .5s infinite}
.toast-wrap{position:fixed;bottom:24px;left:50%;transform:translateX(-50%);z-index:9999;pointer-events:none;display:flex;justify-content:center}
.toast{padding:12px 16px;border-radius:var(--r-sm);font-size:13px;background:var(--ink);color:var(--paper);opacity:0;transform:translateY(20px);transition:opacity .25s ease,transform .25s ease;font-family:'DM Mono',monospace;display:block;box-shadow:0 8px 20px rgba(0,0,0,.12);min-width:220px;text-align:center}
.toast.show{opacity:1;transform:translateY(0)}
.toast.err{background:var(--red);color:#fff}

/* ── ABOUT & LICENSE ── */
.about-box,.license-box{background:var(--paper);border:1px solid var(--line2);border-radius:22px;padding:18px;margin-top:18px}
.about-header{display:flex;align-items:center;gap:14px;margin-bottom:14px}
.about-icon{width:56px;height:56px;border-radius:18px;display:flex;align-items:center;justify-content:center;font-size:28px;background:linear-gradient(145deg,#DCFCE7,#ECFDF5);flex-shrink:0}
.about-title{font-size:18px;font-weight:700;color:var(--ink)}
.about-version{font-size:12px;color:var(--ink3);margin-top:2px}
.about-text{margin:0;font-size:14px;line-height:1.8;color:var(--ink2);text-align:justify;text-justify:inter-word}
.license-title{font-size:16px;font-weight:700;color:var(--ink);margin-bottom:10px}
.license-text{margin:0;font-size:13px;line-height:1.8;color:var(--ink3)}
@media(max-width:480px){
  .about-box,.license-box{padding:16px;border-radius:18px}
  .about-icon{width:50px;height:50px;font-size:24px}
  .about-title{font-size:16px}
  .about-text{font-size:13px}
}
</style>
</head>
<body>

<!-- WELCOME OVERLAY -->
<div id="welcome-overlay">
  <div class="welcome-card">
    <div class="welcome-heading">Selamat Datang</div>
    <div class="welcome-logo">
      <svg viewBox="0 0 90 90" xmlns="http://www.w3.org/2000/svg">
        <defs>
          <linearGradient id="welcomeGradient" x1="0%" y1="0%" x2="100%" y2="100%">
            <stop offset="0%" stop-color="#0F5132"/><stop offset="100%" stop-color="#46C487"/>
          </linearGradient>
          <linearGradient id="moonGlow" x1="0%" y1="0%" x2="100%" y2="100%">
            <stop offset="0%" stop-color="#F7E7A1"/><stop offset="100%" stop-color="#D6BF74"/>
          </linearGradient>
        </defs>
        <circle cx="45" cy="45" r="40" fill="url(#welcomeGradient)"/>
        <path d="M28 52C28 36,38 28,45 28C52 28,62 36,62 52L62 66L28 66Z" fill="white"/>
        <rect x="22" y="38" width="5" height="28" rx="2" fill="white"/>
        <rect x="63" y="38" width="5" height="28" rx="2" fill="white"/>
        <path d="M54 18A8 8 0 1 1 47 31A5.5 5.5 0 1 0 54 18" fill="url(#moonGlow)"/>
        <path d="M18 74C30 66,42 82,54 74C64 68,74 78,82 72" stroke="#D7F9E2" stroke-width="4" fill="none" stroke-linecap="round"/>
      </svg>
    </div>
    <div class="welcome-title">Freschtive</div>
    <div class="welcome-sub">Smart Mosque Fragrance System</div>
    <div class="welcome-status">Fragrance • Schedule • Adaptive</div>
    <div class="welcome-loader"><span></span><span></span><span></span></div>
  </div>
</div>
<header>
  <div class="brand">
    <div class="brand-logo">
      <svg viewBox="0 0 460 110" xmlns="http://www.w3.org/2000/svg">
        <defs>
          <linearGradient id="freshGradient" x1="0%" y1="0%" x2="100%" y2="100%">
            <stop offset="0%" stop-color="#0F5132"/><stop offset="100%" stop-color="#46C487"/>
          </linearGradient>
          <linearGradient id="goldGlow" x1="0%" y1="0%" x2="100%" y2="100%">
            <stop offset="0%" stop-color="#E8D38A"/><stop offset="100%" stop-color="#F6E7A1"/>
          </linearGradient>
          <filter id="softShadow">
            <feDropShadow dx="0" dy="2" stdDeviation="3" flood-opacity="0.18"/>
          </filter>
        </defs>
        <g transform="translate(10,10)">
          <circle cx="45" cy="45" r="38" fill="url(#freshGradient)" filter="url(#softShadow)"/>
          <path d="M28 52 C28 35,38 26,45 26 C52 26,62 35,62 52 L62 66 L28 66Z" fill="white"/>
          <rect x="23" y="38" width="5" height="28" rx="2" fill="white"/>
          <rect x="62" y="38" width="5" height="28" rx="2" fill="white"/>
          <path d="M54 18 A9 9 0 1 1 47 32 A6 6 0 1 0 54 18" fill="url(#goldGlow)"/>
          <path d="M18 74 C30 66,42 82,54 74 C64 68,74 78,82 72" stroke="#CFF6DD" stroke-width="4" fill="none" stroke-linecap="round"/>
        </g>
        <text x="95" y="58" font-family="Trebuchet MS, Arial, sans-serif" font-size="42" font-weight="700" letter-spacing="-1" fill="url(#freshGradient)" filter="url(#softShadow)">Freschtive</text>
        <path d="M315 28 C325 12,345 12,350 28 C345 42,325 44,315 28Z" fill="#3AB874" transform="rotate(15 333 28)"/>
        <circle cx="340" cy="46" r="3" fill="#A6E9C6"/><circle cx="353" cy="38" r="2" fill="#D8FFE7"/>
        <line x1="98" y1="67" x2="345" y2="67" stroke="#D7E8DD" stroke-width="1"/>
        <text x="100" y="86" font-family="Arial, sans-serif" font-size="11" letter-spacing="2" fill="#6B7280">FRAGRANCE • SCHEDULE • ADAPTIVE</text>
      </svg>
    </div>
  </div>
  <div class="conn-pill"><span class="conn-dot"></span>192.168.4.1</div>
</header>

<div class="clock-section">
  <div class="time-display mono" id="jamSekarang">--:--:--</div>
  <div class="date-display" id="tanggal">memuat...</div>
</div>

<div class="stat-row">
  <div class="stat">
    <div class="stat-label">SENSOR MQ-135</div>
    <div class="stat-value mono ok" id="nilaiPPM">---</div>
    <div class="stat-unit">raw / 4095</div>
  </div>
  <div class="stat">
    <div class="stat-label">MODE RELAY</div>
    <div style="margin-top:4px">
      <span class="relay-status standby" id="relayStatus"><span class="dot"></span>standby</span>
    </div>
  </div>
</div>

<div class="cooldown-card">
  <div class="cooldown-label">Cooldown Adaptif</div>
  <div id="cooldownText" class="cooldown-value ready">SIAP</div>
</div>

<div class="aq-section">
  <div class="aq-header">
    <span class="aq-label">Kualitas udara</span>
    <span class="aq-badge ok" id="aqBadge">Baik</span>
  </div>
  <div class="bar-track">
    <div class="bar-fill" id="barPPM" style="width:0%;background:var(--green)"></div>
  </div>
  <div class="bar-ticks">
    <span>0</span><span id="tickAmbang">ambang 1000</span><span>4095</span>
  </div>
</div>

<section>
  <div class="section-title">Semprot manual</div>
  <button class="btn btn-primary" onclick="semprotManual()">
    <svg viewBox="0 0 24 24"><path d="M5 12h14M12 5l7 7-7 7"/></svg> Semprot sekarang
  </button>
  <button class="btn btn-ghost" onclick="toggleLog()">Lihat log aktivitas</button>
  <div class="log-box" id="logBox" style="display:none;margin-top:10px">menunggu aktivitas...</div>
</section>

<section>
  <div class="section-title">Jadwal sholat</div>
  <div class="field">
    <label class="field-label">ⓘ Atur waktu sholat untuk semprotan otomatis</label>
  </div>
  <div class="prayer-grid">
    <div><div class="prayer-name">Subuh</div><input type="time" class="prayer-time" id="s0"></div>
    <div><div class="prayer-name">Dzuhur</div><input type="time" class="prayer-time" id="s1"></div>
    <div><div class="prayer-name">Ashar</div><input type="time" class="prayer-time" id="s2"></div>
    <div><div class="prayer-name">Maghrib</div><input type="time" class="prayer-time" id="s3"></div>
    <div><div class="prayer-name">Isya</div><input type="time" class="prayer-time" id="s4"></div>
  </div>
</section>

<section>
  <div class="section-title">Pola semprotan</div>
  <div class="field-row">
    <div class="field">
      <label class="field-label">Jumlah semprotan per waktu sholat</label>
      <input type="number" id="jmlSemprot" min="1" max="10" value="4">
    </div>
    <div class="field">
      <label class="field-label">Jarak waktu antar semprot (menit)</label>
      <input type="number" id="jarakMenit" min="1" max="60" value="10">
    </div>
  </div>
  <div class="field">
    <label class="field-label">Durasi satu semprot (ms)</label>
    <input type="number" id="durasiMs" min="100" max="10000" value="500">
  </div>
</section>

<section>
  <div class="section-title">Sensor adaptif</div>
  <div class="field-row">
    <div class="field">
      <label class="field-label">Ambang batas MQ-135</label>
      <input type="number" id="ambang" min="0" max="4095" value="1000" oninput="updateTick()">
    </div>
    <div class="field">
      <label class="field-label">jarak semprotan adaptif (detik)</label>
      <input type="number" id="cooldown" min="10" max="3600" value="600">
    </div>
  </div>
</section>

<section>
  <div class="section-title">Set waktu RTC</div>
  <div class="info-note">Mode Offline (Access Point). Atur waktu manual sesuai waktu saat ini agar tetap akurat.</div>
  <div class="field">
    <label class="field-label">Waktu sekarang</label>
    <input type="datetime-local" id="inputWaktu">
  </div>
  <button class="btn btn-secondary" onclick="setWaktu()">
    <svg viewBox="0 0 24 24"><circle cx="12" cy="12" r="10"/><polyline points="12 6 12 12 16 14"/></svg> Set waktu ke RTC
  </button>
</section>

<section style="border-bottom:none">
  <button class="btn btn-primary" onclick="simpanSetting()">
    <svg viewBox="0 0 24 24"><polyline points="20 6 9 17 4 12"/></svg> Simpan semua pengaturan
  </button>
</section>

<section class="about-box">
  <div class="about-header">
    <div class="about-icon">
      <svg viewBox="0 0 90 90" xmlns="http://www.w3.org/2000/svg">
        <defs>
          <linearGradient id="welcomeGradient" x1="0%" y1="0%" x2="100%" y2="100%">
            <stop offset="0%" stop-color="#0F5132"/><stop offset="100%" stop-color="#46C487"/>
          </linearGradient>
          <linearGradient id="moonGlow" x1="0%" y1="0%" x2="100%" y2="100%">
            <stop offset="0%" stop-color="#F7E7A1"/><stop offset="100%" stop-color="#D6BF74"/>
          </linearGradient>
        </defs>
        <circle cx="45" cy="45" r="40" fill="url(#welcomeGradient)"/>
        <path d="M28 52 C28 36,38 28,45 28 C52 28,62 36,62 52 L62 66 L28 66Z" fill="white"/>
        <rect x="22" y="38" width="5" height="28" rx="2" fill="white"/>
        <rect x="63" y="38" width="5" height="28" rx="2" fill="white"/>
        <path d="M54 18 A8 8 0 1 1 47 31 A5.5 5.5 0 1 0 54 18" fill="url(#moonGlow)"/>
        <path d="M18 74 C30 66,42 82,54 74 C64 68,74 78,82 72" stroke="#D7F9E2" stroke-width="4" fill="none" stroke-linecap="round"/>
      </svg>
    </div>
    <div>
      <div class="about-title">Tentang Freschtive</div>
      <div class="about-version">Smart Mosque Fragrance v1.0</div>
    </div>
  </div>
  <p class="about-text">
    <b>Freschtive</b> merupakan sistem smart mosque berbasis Internet of Things (IoT) yang dirancang untuk meningkatkan kenyamanan dan kualitas udara di lingkungan masjid melalui teknologi pewangi otomatis dan pemantauan udara secara realtime. Sistem ini mengintegrasikan penjadwalan waktu penyemprotan, mode adaptif berbasis sensor kualitas udara, serta kontrol pintar yang mampu menyesuaikan kondisi lingkungan berdasarkan tingkat kepadatan aktivitas jamaah dan perubahan kualitas udara di dalam ruangan. Dengan pendekatan otomatisasi yang efisien, Freschtive membantu menciptakan suasana masjid yang lebih segar, nyaman, modern, dan mendukung pengalaman ibadah yang lebih khusyuk.
  </p>
</section>

<section class="license-box">
  <div class="license-title">Lisensi & Kepemilikan</div>
  <p class="license-text">© 2026 Freschtive System <br> Developed by <b>FakihDev</b><br> Smart Mosque Air Fragrance Automation</p>
</section>

<div style="height:90px"></div>
<div class="toast-wrap"><div class="toast" id="toast"></div></div>

<script>
const $ = id => document.getElementById(id);
const p = n => String(n).padStart(2, '0');
let logVisible = false;

setTimeout(() => $('welcome-overlay')?.classList.add('hidden'), 2200);

function toggleLog() {
  logVisible = !logVisible;
  $('logBox').style.display = logVisible ? 'block' : 'none';
}

function showToast(msg, ok) {
  const t = $('toast');
  t.textContent = msg;
  t.className = 'toast' + (ok === false ? ' err' : '');
  void t.offsetWidth;
  t.classList.add('show');
  setTimeout(() => t.classList.remove('show'), 2500);
}

function updateTick() {
  $('tickAmbang').textContent = 'ambang ' + ($('ambang').value || '1000');
}

function initWaktu() {
  const now = new Date();
  $('inputWaktu').value = `${now.getFullYear()}-${p(now.getMonth()+1)}-${p(now.getDate())}T${p(now.getHours())}:${p(now.getMinutes())}`;
}

function setWaktu() {
  const val = $('inputWaktu').value;
  if (!val) return showToast('Isi waktu dahulu', false);
  const dt = new Date(val);
  fetch('/api/settime', {
    method: 'POST',
    headers: {'Content-Type': 'application/json'},
    body: JSON.stringify({tahun: dt.getFullYear(), bulan: dt.getMonth()+1, tanggal: dt.getDate(), jam: dt.getHours(), menit: dt.getMinutes(), detik: dt.getSeconds()})
  })
  .then(r => r.json()).then(d => showToast(d.ok ? 'Waktu RTC berhasil diset' : 'Gagal set waktu', d.ok))
  .catch(() => showToast('Koneksi gagal', false));
}

function refresh() {
  fetch('/api/status').then(r => r.json()).then(d => {
    $('jamSekarang').textContent = d.jam;
    $('tanggal').textContent = d.tanggal;
    $('nilaiPPM').textContent = d.ppm;
    
    const ambang = parseInt($('ambang').value) || 1000;
    const pct = Math.min((d.ppm / 4095) * 100, 100);
    const bar = $('barPPM');
    const ppmEl = $('nilaiPPM');
    const badge = $('aqBadge');
    const cd = $('cooldownText');
    
    bar.style.width = pct.toFixed(1) + '%';
    
    if (d.ppm < ambang * 0.6) {
      bar.style.background = 'var(--green)'; ppmEl.className = 'stat-value mono ok';
      badge.textContent = 'Baik'; badge.className = 'aq-badge ok';
    } else if (d.ppm < ambang) {
      bar.style.background = 'var(--amber)'; ppmEl.className = 'stat-value mono warn';
      badge.textContent = 'Sedang'; badge.className = 'aq-badge warn';
    } else {
      bar.style.background = 'var(--red)'; ppmEl.className = 'stat-value mono bad';
      badge.textContent = 'Pengap'; badge.className = 'aq-badge bad';
    }

    if (d.cooldown > 0) {
      cd.textContent = `${Math.floor(d.cooldown/60)}m ${d.cooldown%60}s`;
      cd.className = 'cooldown-value wait';
      cd.style.color = 'var(--amber)';
    } else {
      cd.textContent = 'SIAP';
      cd.className = 'cooldown-value ready';
      cd.style.color = 'var(--green)';
    }
    cd.style.fontWeight = '700';

    if (d.log && logVisible) $('logBox').textContent = d.log.replace(/\\n/g, '\n');
  }).catch(() => {});
}

function muatSetting() {
  fetch('/api/setting').then(r => r.json()).then(d => {
    for (let i = 0; i < 5; i++) $('s'+i).value = `${p(d.jadwal[i][0])}:${p(d.jadwal[i][1])}`;
    $('jmlSemprot').value = d.jumlahSemprot;
    $('jarakMenit').value = d.jarakMenit;
    $('durasiMs').value = d.durasiMs;
    $('ambang').value = d.ambang;
    $('cooldown').value = Math.round(d.cooldown / 1000);
    updateTick();
  }).catch(() => {});
}

function simpanSetting() {
  const jadwal = Array.from({length: 5}, (_, i) => {
    const parts = $('s'+i).value.split(':');
    return [parseInt(parts[0]), parseInt(parts[1])];
  });
  
  fetch('/api/setting', {
    method: 'POST',
    headers: {'Content-Type': 'application/json'},
    body: JSON.stringify({
      jadwal,
      jumlahSemprot: parseInt($('jmlSemprot').value),
      jarakMenit: parseInt($('jarakMenit').value),
      durasiMs: parseInt($('durasiMs').value),
      ambang: parseInt($('ambang').value),
      cooldown: parseInt($('cooldown').value) * 1000
    })
  })
  .then(r => r.json()).then(d => showToast(d.ok ? 'Pengaturan tersimpan' : 'Gagal simpan', d.ok))
  .catch(() => showToast('Koneksi gagal', false));
}

function semprotManual() {
  fetch('/api/spray', {method: 'POST'}).then(r => r.json())
  .then(() => showToast('Semprot berhasil!'))
  .catch(() => showToast('Koneksi gagal', false));
}

initWaktu(); muatSetting(); refresh();
setInterval(refresh, 2000);
</script>
</body>
</html>
)rawhtml";

// ── WEB HANDLERS ────────────────────────────────────────────────
void handleRoot() {
  server.send(200, "text/html", FPSTR(DASHBOARD_HTML));
}

void handleStatus() {
  DateTime now = rtc.now();
  int ppm = analogRead(MQ135_PIN);
  bool relayOn = (digitalRead(RELAY_PIN) == LOW);

  unsigned long sisaCooldown = 0;
  if (millis() - waktuSemprotAdaptifTerakhir < cooldownSemprot) {
    sisaCooldown = (cooldownSemprot - (millis() - waktuSemprotAdaptifTerakhir)) / 1000;
  }

  char jam[10], tanggal[16];
  sprintf(jam, "%02d:%02d:%02d",
          now.hour(), now.minute(), now.second());

  sprintf(tanggal, "%02d/%02d/%04d",
          now.day(), now.month(), now.year());

  String logEsc = logAktivitas;
  logEsc.replace("\n", "\\n");
  logEsc.replace("\"", "'");

  char buf[900];

  snprintf(
    buf,
    sizeof(buf),
    "{\"jam\":\"%s\",\"tanggal\":\"%s\",\"ppm\":%d,\"relay\":%s,\"cooldown\":%lu,\"log\":\"%s\"}",
    jam,
    tanggal,
    ppm,
    relayOn ? "true" : "false",
    sisaCooldown,
    logEsc.c_str()
  );

  server.send(200, "application/json", buf);
}

void handleGetSetting() {
  String json = "{\"jadwal\":[";
  for (int i = 0; i < 5; i++) {
    json += "[" + String(jadwalSholat[i][0]) + "," + String(jadwalSholat[i][1]) + "]";
    if (i < 4) json += ",";
  }
  json += "],\"jumlahSemprot\":"  + String(jumlahSemprot)
        + ",\"jarakMenit\":"     + String(jarakMenit)
        + ",\"durasiMs\":"       + String(durasiSemprot)
        + ",\"ambang\":"         + String(ambangBatasPolusi)
        + ",\"cooldown\":"       + String(cooldownSemprot) + "}";
  server.send(200, "application/json", json);
}

void handlePostSetting() {
  if (!server.hasArg("plain")) { server.send(400, "application/json", "{\"ok\":false}"); return; }
  String body = server.arg("plain");

  // Parse jadwal
  int arrStart = body.indexOf("\"jadwal\":[");
  if (arrStart >= 0) {
    String arrStr = body.substring(arrStart + 10);
    int pos = 0;
    for (int i = 0; i < 5; i++) {
      pos = arrStr.indexOf('[', pos) + 1;
      int end = arrStr.indexOf(']', pos);
      if (end < 0) break;
      String pair = arrStr.substring(pos, end);
      int comma = pair.indexOf(',');
      if (comma < 0) break;
      int jam   = pair.substring(0, comma).toInt();
      int menit = pair.substring(comma + 1).toInt();
      if (jam >= 0 && jam <= 23 && menit >= 0 && menit <= 59) {
        jadwalSholat[i][0] = jam;
        jadwalSholat[i][1] = menit;
        // Reset flag fired saat jadwal berubah
        for (int j = 0; j < 10; j++) lastFiredMinute[i][j] = -1;
      }
      pos = end + 1;
    }
  }

  // Parse nilai lain
  auto getVal = [&](String key) -> long {
    int idx = body.indexOf("\"" + key + "\":");
    if (idx < 0) return -1;
    int start = idx + key.length() + 3;
    int ec = body.indexOf(',', start);
    int eb = body.indexOf('}', start);
    int end = (ec < 0) ? eb : (eb < 0) ? ec : min(ec, eb);
    return body.substring(start, end).toInt();
  };

  long v;
  v = getVal("jumlahSemprot"); if (v > 0)  jumlahSemprot     = v;
  v = getVal("jarakMenit");    if (v > 0)  jarakMenit        = v;
  v = getVal("durasiMs");      if (v > 0)  durasiSemprot     = v;
  v = getVal("ambang");        if (v >= 0) ambangBatasPolusi = v;
  v = getVal("cooldown");      if (v > 0)  cooldownSemprot   = v;

  simpanSetting();
  tambahLog("Setting disimpan");
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleSetTime() {
  if (!server.hasArg("plain")) { server.send(400, "application/json", "{\"ok\":false}"); return; }
  String body = server.arg("plain");

  auto getInt = [&](String key) -> int {
    int idx = body.indexOf("\"" + key + "\":");
    if (idx < 0) return 0;
    int start = idx + key.length() + 3;
    int ec = body.indexOf(',', start);
    int eb = body.indexOf('}', start);
    int end = (ec < 0) ? eb : (eb < 0) ? ec : min(ec, eb);
    return body.substring(start, end).toInt();
  };

  int tahun   = getInt("tahun");
  int bulan   = getInt("bulan");
  int tanggal = getInt("tanggal");
  int jam     = getInt("jam");
  int menit   = getInt("menit");
  int detik   = getInt("detik");

  if (tahun > 2000) {
    rtc.adjust(DateTime(tahun, bulan, tanggal, jam, menit, detik));
    char buf[30];
    sprintf(buf, "Waktu diset %02d:%02d:%02d", jam, menit, detik);
    tambahLog(String(buf));
    // Reset semua flag fired setelah RTC diset ulang
    for (int i = 0; i < 5; i++)
      for (int j = 0; j < 10; j++)
        lastFiredMinute[i][j] = -1;
    server.send(200, "application/json", "{\"ok\":true}");
  } else {
    server.send(400, "application/json", "{\"ok\":false}");
  }
}

void handleSpray() {
  semprotFragrance(durasiSemprot, "Manual Web Dashboard");
  server.send(200, "application/json", "{\"ok\":true}");
}

// ── SETUP ────────────────────────────────────────────────────────
void setup() {
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, HIGH); // Relay OFF (active-low)

  Serial.begin(115200);

  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0); lcd.print("Masjid Air Clean");
  lcd.setCursor(0, 1); lcd.print("Memuat...       ");
  delay(800);

  if (!rtc.begin()) {
    Serial.println("[ERROR] RTC tidak terdeteksi!");
    lcd.setCursor(0, 1); lcd.print("ERROR: RTC!     ");
    delay(2000);
  }

  muatSetting();

  // Inisialisasi flag fired
  for (int i = 0; i < 5; i++)
    for (int j = 0; j < 10; j++)
      lastFiredMinute[i][j] = -1;

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  delay(500);

  IPAddress ip = WiFi.softAPIP();
  Serial.print("[AP] IP: "); Serial.println(ip);

  lcd.clear();
  lcd.setCursor(0, 0); lcd.print(AP_SSID);
  lcd.setCursor(0, 1); lcd.print(ip);

  server.on("/",            HTTP_GET,  handleRoot);
  server.on("/api/status",  HTTP_GET,  handleStatus);
  server.on("/api/setting", HTTP_GET,  handleGetSetting);
  server.on("/api/setting", HTTP_POST, handlePostSetting);
  server.on("/api/settime", HTTP_POST, handleSetTime);
  server.on("/api/spray",   HTTP_POST, handleSpray);
  server.begin();

  tambahLog("Sistem menyala - AP Mode");
}

// ── LOOP ─────────────────────────────────────────────────────────
void loop() {
  server.handleClient(); // tanpa blocking agar responsif

  static unsigned long lastTick = 0;
  if (millis() - lastTick < 200) return;
  lastTick = millis();

  DateTime now = rtc.now();
  int ppm = analogRead(MQ135_PIN);
  int nowEncoded = now.hour() * 60 + now.minute(); // menit absolut hari ini

  // ── Cek jadwal sholat ─────────────────────────────────────────
  // FIX BUG: Gunakan lastFiredMinute agar tidak terlewat meski second() != 0
  // dan tidak duplikat dalam satu menit yang sama.
  for (int i = 0; i < 5; i++) {
    for (int j = 0; j < jumlahSemprot; j++) {
      // Hitung target waktu semprot ke-j untuk sholat ke-i
      int targetTotal = jadwalSholat[i][0] * 60 + jadwalSholat[i][1]
                        - 5 + (j * jarakMenit);
      // Normalisasi rollover (misal subuh lebih awal dari tengah malam)
      if (targetTotal < 0)   targetTotal += 1440;
      if (targetTotal >= 1440) targetTotal -= 1440;

      // Trigger jika menit sekarang == target DAN belum pernah fired menit ini
      if (nowEncoded == targetTotal && lastFiredMinute[i][j] != nowEncoded) {
        lastFiredMinute[i][j] = nowEncoded;
        semprotFragrance(durasiSemprot, String(namaSholat[i]) + " Spray-" + String(j + 1));
      }
    }
  }

  // ── Cek sensor adaptif ────────────────────────────────────────
  if (ppm > ambangBatasPolusi) {
    if (millis() - waktuSemprotAdaptifTerakhir > cooldownSemprot) {
      semprotFragrance(durasiSemprot, "Adaptif PPM=" + String(ppm));
      waktuSemprotAdaptifTerakhir = millis();
    }
  }

  // ── Update LCD ────────────────────────────────────────────────
  char buf[17];
  sprintf(buf, "%02d:%02d  PPM:%-5d", now.hour(), now.minute(), ppm);
  lcd.setCursor(0, 0); lcd.print(buf);
  lcd.setCursor(0, 1); lcd.print("192.168.4.1     ");
}
