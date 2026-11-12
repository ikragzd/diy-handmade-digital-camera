#ifndef SD_HANDLER_H
#define SD_HANDLER_H

#include "FS.h"
#include <Adafruit_ST7735.h>
#include <SD.h>
#include <SPI.h>

#define SD_CS_PIN 21

framesize_t fsize = FRAMESIZE_QQVGA;
extern Adafruit_ST7735 *tft;

bool sd_ready = false;

// SD'yi baslat - bir kere cagrilir
bool mountSD() {
  if (sd_ready)
    return true;
  // SPI hizini 4MHz'e geri cektik
  if (SD.begin(SD_CS_PIN, SPI, 4000000)) {
    sd_ready = true;
    return true;
  }
  return false;
}

// SD'yi pasif birak
void unmountSD() {
  // Bos birakiyoruz, SPI hatti kapatilmaz
}

// ===================== FOTOGRAF =====================

void saveJPGToSD(const uint8_t *buf, size_t len) {
  Serial.printf("[SAVE] JPG kayit basliyor, boyut: %d byte\n", len);

  if (!mountSD()) {
    Serial.println("[SAVE] SD mount basarisiz!");
    tft->setTextSize(1);
    tft->setTextColor(ST7735_RED);
    tft->setCursor(30, 110);
    tft->print("SD Hata!");
    delay(1000);
    return;
  }

  // Benzersiz dosya adi bul
  char fn[32];
  int i = 1;
  while (i <= 9999) {
    sprintf(fn, "/DSC_%04d.JPG", i);
    if (!SD.exists(fn)) {
      char checkVid[32];
      sprintf(checkVid, "/DSC_%04d.AVI", i);
      if (!SD.exists(checkVid))
        break;
    }
    i++;
  }
  Serial.printf("[SAVE] Dosya adi: %s\n", fn);

  delay(10); // SPI bus'in temizlenmesini bekle

  File f = SD.open(fn, "w");

  // Basarisiz olursa SD'yi yeniden baslat ve tekrar dene
  if (!f) {
    Serial.println("[SAVE] Ilk deneme basarisiz, SD yeniden baslatiliyor...");
    SD.end();
    delay(200);
    sd_ready = false;
    if (SD.begin(SD_CS_PIN, SPI, 4000000)) {
      sd_ready = true;
      f = SD.open(fn, "w");
      Serial.printf("[SAVE] Ikinci deneme: %s\n", f ? "basarili" : "basarisiz");
    }
  }

  if (f) {
    size_t written = f.write(buf, len);
    f.close();
    Serial.printf("[SAVE] %d / %d byte yazildi\n", written, len);

    tft->setTextSize(1);
    tft->setTextColor(ST7735_GREEN);
    tft->setCursor(35, 110);
    tft->print("Kaydedildi!");
  } else {
    Serial.println("[SAVE] Dosya acilamadi!");
    tft->setTextSize(1);
    tft->setTextColor(ST7735_RED);
    tft->setCursor(20, 110);
    tft->print("Yazma Hatasi!");
  }
}

// Her kareyi ayrı bir JPG olarak kaydetme fonksiyonu
void saveFrameAsJPG(camera_fb_t *fb, int frameIndex, const char *folderPath) {
  char filename[64];
  sprintf(filename, "%s/fr_%04d.jpg", folderPath, frameIndex);

  // SD kart zaten açık olduğu için ekstra initSD() çağırmaya gerek yok, direkt
  // yazıyoruz:
  File file = SD.open(filename, FILE_WRITE);
  if (file) {
    file.write(fb->buf, fb->len);
    file.close();
  }
}

// ===================== SES (WAV) =====================

// void startWAV() {
//  Gecici olarak SD kart darboğazını engellemek için WAV kaydı devre dışı
//  bırakıldı
//}

/*void appendWAVChunk() {
  // WAV kaydı devre dışı
}

void finishWAV() {
  // WAV kaydı devre dışı
}
*/
#endif
