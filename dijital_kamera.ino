// ============================================================
//  DIJITAL KAMERA - XIAO ESP32S3 Sense + OV3660 + ST7735 TFT
//  Temiz yeniden yazim
// ============================================================

#include "esp_camera.h"
#include "img_converters.h"
#include "icons.h"
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <SPI.h>
#include "sd_handler.h"
//#include "driver/i2s.h"
#include <algorithm>
#include <vector>

void batTask(void *pvParameters);
void drawOSD();

// ======================== PIN TANIMLARI ========================

// TFT Ekran
#define TFT_CS   1
#define TFT_DC   2
#define TFT_MOSI 9
#define TFT_MISO 8
#define TFT_SCLK 7
#define TFT_RST  -1

// Kamera (XIAO ESP32S3 Sense)
#define CAM_PWDN  -1
#define CAM_RST   -1
#define CAM_XCLK  10
#define CAM_SDA   40
#define CAM_SCL   39
#define CAM_D0    15
#define CAM_D1    17
#define CAM_D2    18
#define CAM_D3    16
#define CAM_D4    14
#define CAM_D5    12
#define CAM_D6    11
#define CAM_D7    48
#define CAM_VSYNC 38
#define CAM_HREF  47
#define CAM_PCLK  13

// Butonlar
#define BTN_DOWN    43
#define BTN_LEFT    5
#define BTN_RIGHT   4
#define BTN_OK      6
#define BTN_SHUTTER 44

// Mikrofon (I2S PDM)
//#define MIC_WS   42
//#define MIC_DATA 41

// Pil
#define BAT_PIN 3

// ======================== DURUMLAR ========================

enum State {
  ST_LIVE,        // Canli kamera goruntusu
  ST_MENU,        // Alt menu (filtre + galeri)
  ST_FILTER,      // Filtre secimi
  ST_GALLERY,     // Galeri gorunumu
  ST_PHOTO_VIEW,  // Cekilen fotograf gosterimi (donmus ekran)
  ST_VIDEO        // Video kaydi
};

State state = ST_LIVE;

// ======================== GLOBAL DEGISKENLER ========================

Adafruit_ST7735 *tft = NULL;

// Menu
int menuSel = 0; // 0=Filtre, 1=Galeri

// Filtreler
int curFilter = 0;
const char* filterName[] = {"Normal", "Canli", "S/B", "Retro", "Vintage", "Sinema", "Sicak", "Soguk"};
unsigned long filterShowTime = 0;

// Pil
unsigned long batTimer = 0;
int batPct = -1;

// Fotograf
unsigned long photoFreezeTime = 0;

// Video
bool recording = false;
int frameIndex = 0;
unsigned long lastFrameTime = 0;
const int targetInterval = 50;
unsigned long recBlinkTimer = 0;
bool recDot = false;
  int videoKlasorSayaci = 0;
char aktifKlasor[32];
unsigned long videoBaslangicZamani = 0;
int videoSuresiSaniye = 0;


// Galeri
std::vector<String> galFiles;
int galIdx = 0;
bool galPlaying = false;
uint16_t *galBuf = NULL;

// ======================== BUTON SINIFI ========================

struct Btn {
  bool cur = false;
  unsigned long t = 0;
  bool shortEvt = false;
  bool longEvt = false;
  bool longDone = false;

  void read(bool pressed) {
    if (pressed != cur) {
      if (millis() - t > 50) {
        cur = pressed;
        t = millis();
        if (cur) { shortEvt = true; longDone = false; }
      }
    } else if (cur && !longDone && millis() - t > 1000) {
      longEvt = true;
      longDone = true;
      shortEvt = false;
    }
  }

  bool tap() {
    if (shortEvt && !cur) { shortEvt = false; return true; }
    return false;
  }

  bool hold() {
    if (longEvt) { longEvt = false; return true; }
    return false;
  }
};

Btn bDown, bLeft, bRight, bOK, bShutter;

// ======================== YARDIMCI FONKSIYONLAR ========================

// Byte swap (kamera big-endian, ekran little-endian)
void bswap(uint16_t *p, int n) {
  for (int i = 0; i < n; i++)
    p[i] = (p[i] << 8) | (p[i] >> 8);
}

// 1-bit ikon ciz (seffaf arka plan, sadece siyah pikseller)
void drawIcon1bit(int x, int y, const uint8_t *bmp, int w, int h, uint16_t col) {
  int bw = (w + 7) / 8;
  tft->startWrite();
  for (int j = 0; j < h; j++)
    for (int i = 0; i < w; i++)
      if (!(pgm_read_byte(bmp + j * bw + i / 8) & (128 >> (i & 7))))
        tft->writePixel(x + i, y + j, col);
  tft->endWrite();
}

// 16-bit RGB565 ikon ciz (0xFFFF = seffaf)
void drawIconRGB(int x, int y, const uint16_t *bmp, int w, int h) {
  tft->startWrite();
  for (int j = 0; j < h; j++)
    for (int i = 0; i < w; i++) {
      uint16_t c = pgm_read_word(bmp + j * w + i);
      if (c != 0xFFFF) tft->writePixel(x + i, y + j, c);
    }
  tft->endWrite();
}

// ======================== KAMERA FILTRESI ========================

void applyFilter() {
  sensor_t *s = esp_camera_sensor_get();
  if (!s) return;

  // Sifirla
  s->set_wb_mode(s, 0); // Beyaz ayarini otomatiğe al
  s->set_brightness(s, 0);
  s->set_contrast(s, 0);
  s->set_saturation(s, 0);
  s->set_special_effect(s, 0);

  switch (curFilter) {
    case 1: // Canli
      s->set_brightness(s, 1);
      s->set_contrast(s, 1);
      s->set_saturation(s, 2);
      break;
    case 2: // Siyah-Beyaz
      s->set_contrast(s, 1);
      s->set_special_effect(s, 2);
      break;
    case 3: // Retro (Sepya)
      s->set_brightness(s, 1);
      s->set_saturation(s, -1);
      s->set_special_effect(s, 6);
      break;
    case 4: // Vintage
      s->set_brightness(s, 1);
      s->set_contrast(s, -2);
      s->set_saturation(s, -2);
      break;
    case 5: // Sinema (Yuksek Kontrast, Hafif Renk Kaybi)
      s->set_brightness(s, -1);
      s->set_contrast(s, 2);
      s->set_saturation(s, -1);
      break;
    case 6: // Sicak
      s->set_wb_mode(s, 2); // Cloudy (Sari/Sicak Beyaz Ayari)
      s->set_saturation(s, 1);
      break;
    case 7: // Soguk
      s->set_wb_mode(s, 3); // Office (Mavi/Soguk Beyaz Ayari)
      s->set_saturation(s, 1);
      break;
  }
}

// ======================== GALERI ========================

int totalFiles = 0; // Tani icin

void loadGallery() {
  galFiles.clear();
    totalFiles = 0;
    
    if (!mountSD()) return;
    
    // İŞTE UNUTTUĞUMUZ / EKSİK OLAN KISIM BURASI:
    File root = SD.open("/");
    if (!root || !root.isDirectory()) { 
        unmountSD(); 
        return; 
    }
    
    root.rewindDirectory();
while (File f = root.openNextFile()) {
    String n = f.name();
    String upperN = n;
    upperN.toUpperCase();

    // EĞER BİR KLASÖRSE VE İSMİ "VID_" İLE BAŞLIYORSA:
    if (f.isDirectory() && upperN.startsWith("VID_")) {
        galFiles.push_back(n); // Klasör adını galeriye ekle
    }
    // EĞER NORMAL BİR FOTOĞRAFSA:
    else if (!f.isDirectory() && upperN.endsWith(".JPG")) {
        galFiles.push_back(n); // Fotoğraf adını galeriye ekle
    }
    f.close();
}
  // En son cekilen (numarasi en buyuk olan) en ilk gorunecek sekilde sirala (descending)
  std::sort(galFiles.begin(), galFiles.end(),
            [](const String &a, const String &b) { return a > b; });
  unmountSD();
}

// ======================== EKRAN BASLATMA ========================

void initScreen() {
  tft->initR(INITR_GREENTAB);
  tft->setRotation(3);
  // Renk duzeltme: JPEG decode ve eski ham RGB565 ayni MADCTL degerini kullaniyor
  // 0x60 = MX | MV (RGB modu)
  uint8_t madctl = 0x60;
  tft->sendCommand(0x36, &madctl, 1);
  // Gamma ayarlari
  const uint8_t gp[] = {0x0f,0x1a,0x0f,0x18,0x2f,0x28,0x20,0x22,0x1f,0x1b,0x23,0x37,0x00,0x07,0x02,0x10};
  const uint8_t gn[] = {0x0f,0x1b,0x0f,0x17,0x33,0x2c,0x29,0x2e,0x30,0x30,0x39,0x3f,0x00,0x07,0x03,0x10};
  tft->sendCommand(0xE0, gp, 16);
  tft->sendCommand(0xE1, gn, 16);
}

// ======================== SETUP ========================

void setup() {
  Serial.begin(115200);
  Serial.println("Sistem basliyor...");

  // --- Ozel SD Kart Testi (Kamera RAM'i isgal etmeden once) ---
  SPI.begin(7, 8, 9, -1);
  tft = new Adafruit_ST7735(&SPI, TFT_CS, TFT_DC, TFT_RST);
  initScreen();
  uint8_t d = 0x60;
  tft->sendCommand(0x36, &d, 1); // RGB modu
  if (SD.begin(21, SPI, 4000000)) {
    sd_ready = true;
  }

  // Buton pinleri
  pinMode(BTN_DOWN, INPUT_PULLUP);
  pinMode(BTN_LEFT, INPUT_PULLUP);
  pinMode(BTN_RIGHT, INPUT_PULLUP);
  pinMode(BTN_OK, INPUT_PULLUP);
  pinMode(BTN_SHUTTER, INPUT_PULLUP);
  // -----------------------------------------------------------

  // SD kütüphanesi CS pinini kendisi yönetir, elle müdahale etmiyoruz.
  // Ekran zaten baslatildi, sadece temizliyoruz
  tft->fillScreen(ST7735_BLACK);
  tft->setTextColor(ST7735_WHITE);
  tft->setCursor(15, 55);
  tft->setTextSize(1);
  tft->println("Kamera Aciliyor...");

  // Kamera Yapilandirmasi
  camera_config_t cfg;
  cfg.ledc_channel = LEDC_CHANNEL_0;
  cfg.ledc_timer   = LEDC_TIMER_0;
  cfg.pin_d0       = CAM_D0;
  cfg.pin_d1       = CAM_D1;
  cfg.pin_d2       = CAM_D2;
  cfg.pin_d3       = CAM_D3;
  cfg.pin_d4       = CAM_D4;
  cfg.pin_d5       = CAM_D5;
  cfg.pin_d6       = CAM_D6;
  cfg.pin_d7       = CAM_D7;
  cfg.pin_xclk     = CAM_XCLK;
  cfg.pin_pclk     = CAM_PCLK;
  cfg.pin_vsync    = CAM_VSYNC;
  cfg.pin_href     = CAM_HREF;
  cfg.pin_sccb_sda = CAM_SDA;
  cfg.pin_sccb_scl = CAM_SCL;
  cfg.pin_pwdn     = CAM_PWDN;
  cfg.pin_reset    = CAM_RST;
  cfg.xclk_freq_hz = 10000000;
  cfg.pixel_format = PIXFORMAT_JPEG; // RAW yerine JPEG kullaniyoruz
  cfg.frame_size   = FRAMESIZE_UXGA; // PSRAM uzerinde maksimum bellek ayirmasi icin en yuksek cozunurlukte baslat
  cfg.jpeg_quality = 10;             // Yuksek kalite
  cfg.fb_count     = 1;              // 1 Frame Buffer yeterli
  cfg.fb_location  = CAMERA_FB_IN_PSRAM; // Tamamen PSRAM kullanmaliyiz

  delay(500);

  if (esp_camera_init(&cfg) != ESP_OK) {
    tft->fillScreen(ST7735_RED);
    tft->setCursor(5, 55);
    tft->println("Kamera Basarisiz!");
    while (1);
  }

  // OV3660 yonunu duzelt (Eger ters gosteriyorsa buradaki 0 ve 1 degerlerini degistirebilirsiniz)
  sensor_t *s = esp_camera_sensor_get();
  if (s) {
    s->set_vflip(s, 1); // 0 veya 1
    s->set_hmirror(s, 0); // 0 veya 1
    
    // Live View icin cozunurlugu QVGA (320x240) seviyesine dusur
    s->set_framesize(s, FRAMESIZE_QVGA);
  }

  // --- Mikrofon I2S Baslatma ---
 /* i2s_config_t ic = {
      .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_PDM),
      .sample_rate = 16000,
      .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
      .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
      .communication_format = I2S_COMM_FORMAT_STAND_I2S,
      .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
      .dma_buf_count = 4,
      .dma_buf_len = 1024,
      .use_apll = false,
      .tx_desc_auto_clear = false,
      .fixed_mclk = 0};
  i2s_pin_config_t ip = {
      .bck_io_num = I2S_PIN_NO_CHANGE, // PDM'de BCK kullanilmaz
      .ws_io_num = MIC_WS,             // CLK pini
      .data_out_num = I2S_PIN_NO_CHANGE,
      .data_in_num = MIC_DATA};        // DATA pini
  i2s_driver_install(I2S_NUM_0, &ic, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &ip);
  i2s_set_clk(I2S_NUM_0, 16000, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_MONO); */

  // Galeri ve Live View Buffer
  galBuf = (uint16_t *)malloc(160 * 120 * 2);
  if (!galBuf) galBuf = (uint16_t *)heap_caps_malloc(160 * 120 * 2, MALLOC_CAP_SPIRAM);
  Serial.printf("galBuf: %s (%p)\n", galBuf ? "OK" : "HATA", galBuf);

  xTaskCreatePinnedToCore(batTask, "BatTask", 2048, NULL, 1, NULL, 1);
}

// ======================== CANLI YAYIN ========================

void drawLiveFrame() {
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) return;
  
  // Kameradan gelen QVGA (320x240) JPEG dosyasini donanimsal olarak 160x120 RGB565 formatina cevir
  bool ok = jpg2rgb565(fb->buf, fb->len, (uint8_t*)galBuf, JPG_SCALE_2X);
  
  if (ok) {
    tft->drawRGBBitmap(0, (tft->height() - 120) / 2, galBuf, 160, 120);
  }
  
  esp_camera_fb_return(fb);
}

// ======================== GALERI GORUNUMU ========================

void showGalleryItem() {
    tft->fillScreen(ST7735_BLACK);
    
    if (galFiles.empty()) {
        tft->setTextSize(1);
        tft->setTextColor(ST7735_WHITE);
        tft->setCursor(20, 60);
        tft->print("Galeri Bos!");
        return;
    }

    String secilenOge = galFiles[galIdx];
    bool isVideo = secilenOge.startsWith("vid_");

    // EĞER SEÇİLEN ÖĞE BİR VİDEO KLASÖRÜ İSE (Fotoğraf okumayla hiç uğraşma):
    if (isVideo) {
        // 1. Şık bir arka plan kutusu çiz (Gri veya koyu mavi bir çerçeve)
        tft->drawRect(20, (tft->height() - 90) / 2, 120, 90, ST7735_BLUE);
        tft->fillRect(22, ((tft->height() - 90) / 2) + 2, 116, 86, ST7735_BLACK);

        // 2. Üzerine büyükçe bir "VIDEO" yazısı yaz
        tft->setTextSize(1);
        tft->setTextColor(ST7735_WHITE);
        tft->setCursor(55, (tft->height() / 2) - 25);
        tft->print("VİDEO");

        // 3. Tam ortaya koca bir Play (Oynat) üçgeni ve daire koy
        int cx = tft->width() / 2;
        int cy = tft->height() / 2 + 5;
        tft->fillTriangle(cx - 8, cy - 12,  cx - 8, cy + 12,  cx + 12, cy, ST7735_WHITE);
        tft->drawCircle(cx, cy, 18, ST7735_WHITE);
        
        // 4. Altına klasör adını (Örn: vid_1) yaz
        tft->setCursor(50, (tft->height() / 2) + 30);
        tft->print(secilenOge);
    } 
    else {
        // EĞER NORMAL BİR FOTOĞRAFSA (Eski düzgün çalışan fotoğraf çizme kodun burada kalacak):
        if (!mountSD()) return;
        String path = "/" + secilenOge;
        File f = SD.open(path, FILE_READ);
        if (f) {
            size_t fsize = f.size();
            if (fsize > 0 && fsize < 500000) {
                uint8_t *jpgBuf = (uint8_t*)heap_caps_malloc(fsize, MALLOC_CAP_DEFAULT); // veya senin kullandığın malloc
                if (jpgBuf) {
                    f.read(jpgBuf, fsize);
                    f.close();
                    uint16_t *decBuf = (uint16_t*)heap_caps_malloc(200 * 150 * 2, MALLOC_CAP_SPIRAM);
                    if (decBuf) {
                        if (jpg2rgb565(jpgBuf, fsize, (uint8_t*)decBuf, JPG_SCALE_8X)) {
                            for (int y = 0; y < 120; y++) {
                                memcpy(galBuf + y * 160, decBuf + (y + 15) * 200 + 20, 160 * 2);
                            }
                            tft->drawRGBBitmap(0, (tft->height() - 120) / 2, galBuf, 160, 120);
                        }
                        free(decBuf);
                    }
                    free(jpgBuf);
                }
            } else {
                f.close();
            }
        }
        unmountSD();
    }
}

void drawOSD() {
  // Pil yuzdesi (2 saniyede bir guncelle)
  if (millis() - batTimer > 2000 || batPct < 0) {
    uint32_t mv = analogReadMilliVolts(BAT_PIN);
    float raw_v = (mv / 1000.0) * 2.0; // 1/2 voltaj bolucu varsayimiyla gercek voltaj
    
    static float smoothedV = -1;
    if (smoothedV < 0) smoothedV = raw_v; // Ilk okumada degeri direkt al
    else smoothedV = smoothedV * 0.85 + raw_v * 0.15; // Sonraki okumalarda dalgalanmayi filtrele (EMA)
    
    // Geçici voltaj değişkenini siliyoruz, doğrudan ortalamayı kullanıyoruz
    float v = smoothedV;
    
    // LiPo bosalma egrisine gore daha dogru bir yuzde hesabi
    if (v >= 4.15) batPct = 100;
    else if (v <= 3.3) batPct = 0;
    else {
      batPct = (int)(((v - 3.3) / (4.15 - 3.3)) * 100.0);
    }
    
    if (batPct > 100) batPct = 100;
    if (batPct < 0) batPct = 0;
    batTimer = millis();
  }
  
  tft->setTextSize(1);
  tft->setTextColor(ST7735_WHITE); // Siyah arka plani kaldirdik (seffaf oldu)
  tft->setCursor(125, 5);
  tft->print(batPct);
  tft->print("%");

  // Menu ikonu goster
  if (state == ST_MENU) {
    drawIcon1bit(30, 85, icon_filtre, 32, 32, ST7735_BLACK);
    drawIconRGB(98, 85, icon_galeri, 32, 32);

    int sx = (menuSel == 0) ? 30 : 98;
    tft->drawRect(sx - 2, 83, 36, 36, ST7735_WHITE);
  }

  // Filtre ismi goster
  if (state == ST_FILTER && millis() - filterShowTime < 2000) {
    tft->setTextSize(2);
    tft->setTextColor(ST7735_WHITE);
    tft->setCursor(20, 100);
    tft->print(filterName[curFilter]);
    tft->print("    ");
  }
}

// ======================== LOOP ========================

void loop() {
  // Buton okuma
  bDown.read(digitalRead(BTN_DOWN) == LOW);
  bLeft.read(digitalRead(BTN_LEFT) == LOW);
  bRight.read(digitalRead(BTN_RIGHT) == LOW);
  bOK.read(digitalRead(BTN_OK) == LOW);
  bShutter.read(digitalRead(BTN_SHUTTER) == LOW);

  // =============== FOTOGRAF GORUNTULEME (donmus ekran) ===============
  if (state == ST_PHOTO_VIEW) {
    // Ekran donmus, sadece sure bitince canli yayina don
    if (millis() - photoFreezeTime > 2000) {
      state = ST_LIVE;
    }
    return; // Hic bir sey cizme, ekran donmus!
  }

  // =============== VIDEO KAYDI ===============
  if (state == ST_VIDEO && recording) {
    // Durdurma: SHUTTER veya DOWN butonuna bas
    if (bShutter.tap() || bDown.tap()) {
      recording = false;
    
    }
   else {
        // 2. Kayıt devam ederken 15-20 FPS hızında kare yakala ve kaydet
        unsigned long currentMillis = millis();
        if (currentMillis - lastFrameTime >= targetInterval) {
            lastFrameTime = currentMillis;
            
            camera_fb_t *fb = esp_camera_fb_get();
            if (fb) {
                // EKRANDA CANLI ÖNİZLEME (Canlı çekim durmaz)
                if (jpg2rgb565(fb->buf, fb->len, (uint8_t*)galBuf, JPG_SCALE_2X)) {
                    tft->drawRGBBitmap(0, (tft->height() - 120) / 2, galBuf, 160, 120);
                }
                
                // SD KARTA SERİ JPG OLARAK YAZ
               // 559. satırdaki çağrıyı şu şekilde güncelle:
saveFrameAsJPG(fb, frameIndex, aktifKlasor);
                frameIndex++;
                
                esp_camera_fb_return(fb);
            }
        } } 
    // Kayıt bittiğinde çağrılacak kısım:
    if (!recording) {
     unmountSD();
     // finishWAV();
      tft->fillScreen(ST7735_BLACK);
      state = ST_LIVE;
      return; }

       if (millis() - recBlinkTimer > 500) {
      recDot = !recDot;
      recBlinkTimer = millis();
    }
    if (recDot) tft->fillCircle(15, 15, 6, ST7735_RED);
    videoSuresiSaniye = (millis() - videoBaslangicZamani) / 1000;

// Ekrana yazdır (Örneğin sol üst köşeye)
tft->setCursor(30, 15);
tft->setTextSize(1);
tft->setTextColor(ST7735_WHITE);
tft->print(videoSuresiSaniye);
tft->print("s");

    }

    // Kare al -> ekrana ciz (performans icin 5 karede 1 ciz) -> SD'ye yaz
   

    // Yanip sonen kirmizi nokta
   
   // appendWAVChunk();
 
  
  // =============== MENU KONTROLLERI ===============

  if (state == ST_LIVE) {
    if (bDown.tap()) state = ST_MENU;

  } else if (state == ST_MENU) {
    if (bDown.tap()) state = ST_LIVE;
    if (bLeft.tap() || bRight.tap()) {
      menuSel = 1 - menuSel; // 0<->1 gecis
    }
    if (bOK.tap()) {
      if (menuSel == 0) {
        state = ST_FILTER;
        filterShowTime = millis();
      } else {
        state = ST_GALLERY;
        tft->fillScreen(ST7735_BLACK);
        loadGallery();
        showGalleryItem();
      }
    }

  } else if (state == ST_FILTER) {
    if (bOK.tap() || bDown.tap()) state = ST_MENU;
    if (bLeft.tap()) {
      curFilter = (curFilter + 1) % 8;
      applyFilter();
      filterShowTime = millis();
    }
    if (bRight.tap()) {
      curFilter = (curFilter - 1 + 8) % 8;
      applyFilter();
      filterShowTime = millis();
    }

  } else if (state == ST_GALLERY) {
    if (!galPlaying) {
      if (bLeft.tap() && !galFiles.empty()) {
        galIdx = (galIdx + 1) % (int)galFiles.size();
        showGalleryItem();
      }
      if (bRight.tap() && !galFiles.empty()) {
        galIdx = (galIdx - 1 + (int)galFiles.size()) % (int)galFiles.size();
        showGalleryItem();
      }
      if (bDown.tap()) {
        state = ST_LIVE;
        tft->fillScreen(ST7735_BLACK);
        return;
      }
      // Sil
      if (bShutter.hold() && !galFiles.empty()) {
        String del = galFiles[galIdx];
        if (!del.startsWith("/")) del = "/" + del;
        if (mountSD()) {
          SD.remove(del);
          unmountSD();
          tft->setTextColor(ST7735_RED);
          tft->setTextSize(2);
          tft->setCursor(30, 50);
          tft->print("Silindi");
          delay(800);
          loadGallery();
          if (galIdx >= (int)galFiles.size() && !galFiles.empty())
            galIdx = galFiles.size() - 1;
          showGalleryItem();
        }
      }
      
      // Kullanilmayan buton eventlerini temizle ki baska ekrana gecince tetiklenmesin
      bShutter.tap(); 
      bOK.tap();
      
    }
    return; // Galeri ekranindayken canli yayini cizmemek icin donguden cik!
  }

  // =============== UZUN BASMA -> VIDEO BASLAT ===============

// Uzun basılıp video başlatıldığı an:
if (bShutter.hold() && (state == ST_LIVE || state == ST_MENU || state == ST_FILTER)) {
  videoBaslangicZamani = millis(); // Kronometreyi başlat
    state = ST_VIDEO;
    recording = true;
    frameIndex = 0;
    
    // Her videoya özel benzersiz bir klasör ismi üret (Örn: /vid_0, /vid_1...)
    videoKlasorSayaci++;
    sprintf(aktifKlasor, "/vid_%d", videoKlasorSayaci);
    
    // ESP32'ye klasör oluşturma komutu:
    if (mountSD()) {
        SD.mkdir(aktifKlasor); // Klasörü SD karta oluşturur
    }
    return;
}

  // =============== KISA BASMA -> FOTOGRAF CEK ===============
  if (bShutter.tap() && (state == ST_LIVE || state == ST_MENU || state == ST_FILTER)) {
    // Ekranda su an canli goruntu var - dokunma, donmus kalsin!

    sensor_t *s = esp_camera_sensor_get();
    s->set_framesize(s, FRAMESIZE_UXGA);
    delay(400); // AE/lens stabilizasyon

    // Ilk kareyi oku ve cope at (bozuk / yarim pikselleri temizlemek icin)
    camera_fb_t *fb = esp_camera_fb_get();
    if(fb) esp_camera_fb_return(fb);

    // Asil fotografi cek
    fb = esp_camera_fb_get();
    if (fb) {
      saveJPGToSD(fb->buf, fb->len);
      esp_camera_fb_return(fb);
    }

    s->set_framesize(s, FRAMESIZE_QVGA);
    state = ST_PHOTO_VIEW;
    photoFreezeTime = millis();
    return;
  }

  // =============== CANLI YAYIN ===============
  if (state == ST_LIVE || state == ST_MENU || state == ST_FILTER) {
    drawLiveFrame();
    drawOSD();
  }
}

// Sadece arkada asili kalmamasi icin bos bir task
void batTask(void *pvParameters) {
  while (1) { vTaskDelay(1000 / portTICK_PERIOD_MS); }
}
