#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <RTClib.h>
#include <WiFi.h>
#include "time.h"

// Kredensial WiFi Masjid
const char* ssid     = "WIFI_MASJID";
const char* password = "aaaaaaaa";

// Konfigurasi NTP Server (Zona Waktu Indonesia Barat GMT+7)
const char* ntpServer       = "id.pool.ntp.org";
const long  gmtOffset_sec   = 25200;
const int   daylightOffset_sec = 0;

// Definisi Pin
#define MQ135_PIN  34
#define RELAY_PIN   4

LiquidCrystal_I2C lcd(0x27, 16, 2);
RTC_DS3231 rtc;

// Jadwal Sholat (Format: Jam, Menit)
int jadwalSholat[5][2] = {
  {4,  45},  // Subuh
  {11, 45},  // Dzuhur
  {15,  0},  // Ashar
  {17, 55},  // Maghrib
  {19, 10}   // Isya
};

int ambangBatasPolusi = 2200;
unsigned long waktuSemprotTerakhir = 0;
const unsigned long cooldownSemprot = 600000; // 10 menit

void setup() {
  Serial.begin(115200);
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, HIGH); // Relay Mati (Active LOW)

  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("Masjid Air Clean");
  lcd.setCursor(0, 1);
  lcd.print("Konek WiFi...");

  WiFi.begin(ssid, password);
  int counter = 0;
  while (WiFi.status() != WL_CONNECTED && counter < 20) {
    delay(500);
    counter++;
  }

  if (!rtc.begin()) {
    Serial.println("RTC Tidak Terdeteksi!");
  }

  if (WiFi.status() == WL_CONNECTED) {
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
    }
  }

  lcd.clear();
}

void semprotFragrance(int durasiDetik) {
  lcd.setCursor(0, 1);
  lcd.print("STATUS: SPRAYING");
  digitalWrite(RELAY_PIN, LOW);
  delay(durasiDetik * 1000);
  digitalWrite(RELAY_PIN, HIGH);
  lcd.setCursor(0, 1);
  lcd.print("                "); // Bersihkan baris
}

void loop() {
  DateTime now = rtc.now();
  int nilaiAirQuality = analogRead(MQ135_PIN);

  // 1. Tampilkan jam & kualitas udara di LCD baris pertama
  lcd.setCursor(0, 0);
  if (now.hour()   < 10) lcd.print("0");
  lcd.print(now.hour());
  lcd.print(":");
  if (now.minute() < 10) lcd.print("0");
  lcd.print(now.minute());
  lcd.print(" AirPPM:");
  lcd.print(nilaiAirQuality);
  lcd.print(" ");

  // 2. Logika A: Semprot 15 menit sebelum adzan
  for (int i = 0; i < 5; i++) {
    int targetJam   = jadwalSholat[i][0];
    int targetMenit = jadwalSholat[i][1] - 15;

    if (targetMenit < 0) {
      targetJam   -= 1;
      targetMenit += 60;
    }

    if (now.hour()   == targetJam   &&
        now.minute() == targetMenit &&
        now.second() == 0) {
      Serial.println("Jadwal: Menyemprotkan wewangian persiapan sholat...");
      semprotFragrance(4);
      delay(1000);
    }
  }

  // 3. Logika B: Semprot adaptif jika udara pengap
  if (nilaiAirQuality > ambangBatasPolusi) {
    if (millis() - waktuSemprotTerakhir > cooldownSemprot) {
      Serial.println("Adaptif: Udara pengap, menyemprotkan...");
      semprotFragrance(2);
      waktuSemprotTerakhir = millis();
    }
  }

  lcd.setCursor(0, 1);
  lcd.print("STATUS: STANDBY ");
  delay(500);
}