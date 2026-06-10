
//  MASJID AIR CLEAN — Sistem Pewangi Otomatis
//  Hardware : ESP32 DevKit + RTC DS3231 + MQ-135 + Relay 5V + LCD I2C
//  Author   : (nama anda)
//  Versi    : 2.0


#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <RTClib.h>
#include <WiFi.h>
#include "time.h"


//  [1] TOGGLE MODE — ganti true/false sebelum upload
//      true  = mode pengujian (parameter ringan, semprot manual)
//      false = mode aktif masjid (jadwal & parameter sesungguhnya)
#define MODE_TEST true



//  [2] KONFIGURASI HARDWARE
#define MQ135_PIN  34   // Pin ADC sensor kualitas udara MQ-135
#define RELAY_PIN   4   // Pin kendali relay dispenser (Active LOW)

LiquidCrystal_I2C lcd(0x27, 16, 2);  // LCD 16x2 alamat I2C 0x27
RTC_DS3231 rtc;                        // Modul RTC untuk pembacaan waktu


//  [3] KONFIGURASI JARINGAN & NTP
const char* ssid             = "tethering";       // Nama WiFi
const char* password         = "aaaaaaaa";         // Password WiFi
const char* ntpServer        = "id.pool.ntp.org";  // Server NTP Indonesia
const long  gmtOffset_sec    = 25200;              // UTC+7 (WIB) = 7 x 3600
const int   daylightOffset_sec = 0;                // Indonesia tidak pakai DST


//  [4A] JADWAL & PARAMETER — MODE TEST
//       Gunakan waktu mendekati jam sekarang agar mudah diverifikasi
//       Semprot dipicu 5 menit sebelum & setiap 5 menit (4x total)
#if MODE_TEST

int jadwalSholat[5][2] = {
  { 4, 45},   // Subuh   (test: sesuaikan jam ini dengan waktu sekarang)
  {11, 45},   // Dzuhur
  {15,  0},   // Ashar
  {17, 55},   // Maghrib
  {2, 15}    // Isya
};

int           ambangBatasPolusi = 1000;    // Rendah agar sensor mudah terpicu
unsigned long cooldownSemprot   = 30000;  // Jeda antar semprot adaptif: 30 detik
unsigned long durasiJadwal      = 500;    // Durasi semprot jadwal: 500ms
unsigned long durasiAdaptif     = 500;   // Durasi semprot adaptif: 500ms


//  [4B] JADWAL & PARAMETER — MODE AKTIF MASJID
#else

int jadwalSholat[5][2] = {
  { 4, 45},   // Subuh
  {11, 45},   // Dzuhur
  {15,  0},   // Ashar
  {17, 55},   // Maghrib
  {19, 10}    // Isya
};

int           ambangBatasPolusi = 1000;   // Ambang normal kualitas udara MQ-135
unsigned long cooldownSemprot   = 600000; // Jeda antar semprot adaptif: 10 menit
unsigned long durasiJadwal      = 500;   // Durasi semprot jadwal: 500ms
unsigned long durasiAdaptif     = 500;   // Durasi semprot adaptif: 500ms

#endif


// ================================================================
//  [5] VARIABEL GLOBAL
// ================================================================
unsigned long waktuSemprotTerakhir = 0;  // Timestamp semprot adaptif terakhir


// ================================================================
//  [6] FUNGSI: Aktifkan relay dispenser selama durasiMs milidetik
// ================================================================
void semprotFragrance(unsigned long durasiMs, String alasan) {
  Serial.print("[SEMPROT] "); Serial.print(alasan);
  Serial.print(" | durasi: "); Serial.print(durasiMs); Serial.println("ms");

  lcd.setCursor(0, 1);
  lcd.print("STATUS: SPRAYING");

  digitalWrite(RELAY_PIN, LOW);   // Relay ON (Active LOW)
  delay(durasiMs);
  digitalWrite(RELAY_PIN, HIGH);  // Relay OFF

  lcd.setCursor(0, 1);
  lcd.print("STATUS: STANDBY ");
}


// ================================================================
//  [7] FUNGSI: Baca & proses perintah dari Serial Monitor
//      Hanya aktif saat MODE_TEST = true
//      T = semprot manual | I = info sensor | R = reset cooldown
// ================================================================
#if MODE_TEST
void cekSerialMonitor() {
  if (!Serial.available()) return;

  char perintah = Serial.read();
  while (Serial.available()) Serial.read(); // buang sisa newline

  switch (perintah) {

    case 'T': case 't':
      Serial.println("[TEST] Semprot manual dipicu!");
      semprotFragrance(durasiJadwal, "Manual via Serial Monitor");
      break;

    case 'I': case 'i': {
      DateTime now = rtc.now();
      int val      = analogRead(MQ135_PIN);
      long sisa    = (long)cooldownSemprot - (long)(millis() - waktuSemprotTerakhir);

      Serial.println("--- INFO SEKARANG ---");
      Serial.print("  Waktu  : ");
      if (now.hour()   < 10) Serial.print("0"); Serial.print(now.hour());   Serial.print(":");
      if (now.minute() < 10) Serial.print("0"); Serial.print(now.minute()); Serial.print(":");
      if (now.second() < 10) Serial.print("0"); Serial.println(now.second());
      Serial.print("  AirPPM : "); Serial.println(val);
      Serial.print("  Ambang : "); Serial.println(ambangBatasPolusi);
      Serial.print("  Cooldown tersisa: ");
      Serial.print(sisa > 0 ? sisa / 1000 : 0); Serial.println(" detik");
      Serial.println("---------------------");
      break;
    }

    case 'R': case 'r':
      waktuSemprotTerakhir = 0;
      Serial.println("[RESET] Cooldown semprot direset ke 0!");
      break;

    default:
      Serial.println("[?] Perintah tidak dikenal. Gunakan: T / I / R");
      break;
  }
}
#endif


// ================================================================
//  [8] SETUP — dijalankan sekali saat ESP32 menyala
// ================================================================
void setup() {

  // -- Relay dimatikan PERTAMA sebelum apapun (cegah semprot saat boot)
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, HIGH);

  // -- Inisialisasi Serial & LCD
  Serial.begin(115200);
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0); lcd.print("Masjid Air Clean");
  lcd.setCursor(0, 1);
  #if MODE_TEST
    lcd.print("** TEST MODE ** ");
  #else
    lcd.print("Menyalakan...   ");
  #endif
  delay(1500);

  // -- Inisialisasi RTC
  if (!rtc.begin()) {
    Serial.println("[ERROR] RTC tidak terdeteksi! Periksa wiring SDA/SCL.");
  }

  // -- Koneksi WiFi (timeout 20 detik)
  lcd.setCursor(0, 1); lcd.print("Konek WiFi...   ");
  WiFi.begin(ssid, password);
  int counter = 0;
  while (WiFi.status() != WL_CONNECTED && counter < 40) {
    delay(500);
    counter++;
  }

  // -- Sinkronisasi RTC dari NTP jika WiFi berhasil
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("[WiFi] Tersambung. Sinkronisasi NTP...");
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) {
      rtc.adjust(DateTime(
        timeinfo.tm_year + 1900,
        timeinfo.tm_mon  + 1,
        timeinfo.tm_mday,
        timeinfo.tm_hour,
        timeinfo.tm_min,
        timeinfo.tm_sec
      ));
      Serial.println("[NTP] RTC berhasil disinkronisasi!");
    } else {
      Serial.println("[NTP] Gagal, pakai waktu RTC lama.");
    }
  } else {
    Serial.println("[WiFi] Gagal konek, pakai waktu RTC lama.");
  }

  lcd.clear();

  // -- Tampilkan panduan Serial Monitor
  Serial.println("============================================");
  Serial.println("   Masjid Air Clean - Sistem Aktif");
  #if MODE_TEST
  Serial.println("   >> MODE TEST AKTIF <<");
  Serial.println("   T = Semprot manual");
  Serial.println("   I = Info sensor & waktu sekarang");
  Serial.println("   R = Reset cooldown semprot");
  #else
  Serial.println("   >> MODE AKTIF MASJID <<");
  #endif
  Serial.println("============================================");
}


// ================================================================
//  [9] LOOP — berjalan terus-menerus
// ================================================================
void loop() {
  DateTime now        = rtc.now();
  int nilaiAirQuality = analogRead(MQ135_PIN);

  // -- [9.1] Baca perintah Serial Monitor (mode test saja)
  #if MODE_TEST
  cekSerialMonitor();
  #endif

  // -- [9.2] Tampilkan jam & nilai PPM di LCD baris atas
  lcd.setCursor(0, 0);
  if (now.hour()   < 10) lcd.print("0"); lcd.print(now.hour());   lcd.print(":");
  if (now.minute() < 10) lcd.print("0"); lcd.print(now.minute());
  lcd.print(" PPM:"); lcd.print(nilaiAirQuality); lcd.print("     ");

// -- [9.3] Logika A: Semprot otomatis 4x pada setiap waktu sholat
//          Pola semprot:
//          - Semprot pertama dimulai 5 menit sebelum adzan
//          - Semprot berikutnya dilakukan setiap 10 menit
//          Contoh (adzan pukul 11:45):
//          11:40 → Spray-1
//          11:50 → Spray-2
//          12:00 → Spray-3
//          12:10 → Spray-4
for (int i = 0; i < 5; i++) {

  int jamSholat   = jadwalSholat[i][0];
  int menitSholat = jadwalSholat[i][1];

  // Loop 4 kali semprotan untuk setiap waktu sholat
  for (int j = 0; j < 4; j++) {

    // Hitung target waktu spray:
    // mulai -5 menit dari adzan,
    // lalu bertambah setiap 10 menit
    int targetJam   = jamSholat;
    int targetMenit = menitSholat - 5 + (j * 10);

    // Koreksi jika menit melebihi 59
    if (targetMenit >= 60) {
      targetJam += targetMenit / 60;
      targetMenit %= 60;
    }

    // Koreksi jika menit bernilai negatif
    if (targetMenit < 0) {
      targetJam--;
      targetMenit += 60;
    }

    // Jalankan spray jika waktu RTC sesuai target
    if (now.hour()   == targetJam   &&
        now.minute() == targetMenit &&
        now.second() == 0) {

      semprotFragrance(
        durasiJadwal,
        "Sholat-" + String(i + 1) +
        " Spray-" + String(j + 1)
      );

      delay(1000); // Hindari trigger ganda pada detik yang sama
    }
  }
}

  // -- [9.4] Logika B: Semprot adaptif jika udara pengap (MQ-135 > ambang)
  if (nilaiAirQuality > ambangBatasPolusi) {
    if (millis() - waktuSemprotTerakhir > cooldownSemprot) {
      semprotFragrance(durasiAdaptif, "Udara pengap PPM=" + String(nilaiAirQuality));
      waktuSemprotTerakhir = millis();
    }
  }

  // -- [9.5] Tampilkan status standby di LCD baris bawah
  lcd.setCursor(0, 1);
  #if MODE_TEST
    lcd.print("TEST:STANDBY    ");
  #else
    lcd.print("STATUS: STANDBY ");
  #endif

  delay(500);
}
