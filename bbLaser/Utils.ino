#include "esp_log.h"
#include "soc/timer_group_struct.h"
#include "soc/timer_group_reg.h"
#include <esp_attr.h>
#define MAXRECORDS 5000
#include <string>

File root;
static const char *TAG = "ilda";
static const char *TMP_FILE = "/bbLaser/product.ild";
const int bufferFrames = 4;
DynamicJsonDocument doc(4096);
JsonArray avaliableMedia = doc.to<JsonArray>();

File tmpFile;
uint8_t fileLength[4];

bool serialBufferReadFlag = true;
uint8_t serialBuffer[2048];
uint serialBufferLen = 0;

uint fileLength_int;
volatile int readLength;
volatile int writeLength;
bool isLoading = false;
bool isWriting = false;
int curMedia = -1;

SemaphoreHandle_t shared_serial_mutex = xSemaphoreCreateMutex();

//=================  Basic Utils -_,-  =========================


void setupSD() {
  if (!SD.begin()) {
    Serial.println("Card Mount Failed");
    return;
  }
  uint8_t cardType = SD.cardType();
  if (cardType == CARD_NONE) {
    Serial.println("No SD card attached");
    return;
  }
  Serial.print("SD Card Type: ");
  if (cardType == CARD_MMC) {
    Serial.println("MMC");
  } else if (cardType == CARD_SD) {
    Serial.println("SDSC");
  } else if (cardType == CARD_SDHC) {
    Serial.println("SDHC");
  } else {
    Serial.println("UNKNOWN");
  }

  uint64_t cardSize = SD.cardSize() / (1024 * 1024);
  Serial.printf("SD Card Size: %lluMB\n", cardSize);
  // SD.remove(TMP_FILE);
  root = SD.open("/bbLaser");
  while (true) {
    File entry = root.openNextFile();
    if (!entry) break;
    if (!entry.isDirectory() && String(entry.name()).indexOf(".ild") != -1) {
      Serial.println(entry.name());
      avaliableMedia.add(String(entry.name()));
    }
  }
}

int buttonState = 0;  //让Core0 和 Core1的操作不要同时出现，不然就读着读着跳下一个文件就Crash了    无操作 -1   上一个 1  下一个 2  自动下一个 3  不要自动 4

void goNext() {
  buttonState = 2;
}

void goPrev() {
  buttonState = 1;
}


//========================================================//


//================== ILDA -_,- ==============================//
typedef struct
{
  char ilda[4];
  uint8_t reserved1[3];
  uint8_t format;
  char frame_name[8];
  char company_name[8];
  volatile uint16_t records;
  uint16_t frame_number;
  uint16_t total_frames;
  uint8_t projector_number;
  uint8_t reserved2; // 帧持续时间
} ILDA_Header_t;

typedef struct
{
  volatile int16_t x;
  volatile int16_t y;
  volatile int16_t z;
  volatile uint8_t status_code;
  volatile uint8_t color;
} ILDA_Record_t;



typedef struct
{
  ILDA_Record_t *records;
  uint16_t number_records;
  bool isBuffered = false;
  uint8_t duration = 0;
} ILDA_Frame_t;

class ILDAFile {
private:
  void dump_header(const ILDA_Header_t &header);

public:
  ILDAFile();
  ~ILDAFile();
  bool read(fs::FS &fs, const char *fname);
  bool tickNextFrame();
  bool parseStream(uint8_t *data, size_t len, int index, int totalLen);
  ILDA_Frame_t *frames;
  volatile int file_frames;
  volatile int cur_frame;
  volatile int cur_frame_render;
  volatile int cur_buffer;
};


//==============   Renderer -_,- ========================//

TaskHandle_t fileBufferHandle;

typedef struct spi_device_t *spi_device_handle_t;  ///< Handle for a device on a SPI bus
class SPIRenderer {
private:
  TaskHandle_t spi_task_handle;
  spi_device_handle_t spi;
  volatile int draw_position;
  volatile int frame_position;

public:
  SPIRenderer();
  void IRAM_ATTR draw();
  void start();
  void reset();
  friend void spi_draw_timer(void *para);
};

#include <cstring>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include <Ticker.h>

#define PIN_NUM_MISO -1
#define PIN_NUM_MOSI 25
#define PIN_NUM_CLK 26
#define PIN_NUM_CS 27
#define PIN_NUM_LDAC 33
#define PIN_NUM_LASER_R 13
#define PIN_NUM_LASER_G 16
#define PIN_NUM_LASER_B 17

SPIRenderer *renderer = NULL;
//==============   Renderer END ========================//

ILDAFile::ILDAFile() {
  frames = NULL;
  file_frames = 0;
  cur_frame = 0;
  cur_frame_render = 0;
  cur_buffer = 0;
}

ILDAFile::~ILDAFile() {
  free(frames);
}

void ILDAFile::dump_header(const ILDA_Header_t &header) {
  char tmp[100];
  strncpy(tmp, header.ilda, 4);
  tmp[5] = '\0';
  ESP_LOGI(TAG, "Header: %s", tmp);
  ESP_LOGI(TAG, "Format Code: %d", header.format);
  strncpy(tmp, header.frame_name, 8);
  tmp[8] = '\0';
  ESP_LOGI(TAG, "Frame Name: %s", tmp);
  strncpy(tmp, header.company_name, 8);
  Serial.println(header.company_name);
  tmp[8] = '\0';
  ESP_LOGI(TAG, "Company Name: %s", tmp);
  ESP_LOGI(TAG, "Number records: %d", header.records);
  ESP_LOGI(TAG, "Number frames: %d", header.total_frames);
}


ILDA_Header_t header;
File file;
unsigned long frameStart;
ILDAFile *ilda = new ILDAFile();

bool ILDAFile::read(fs::FS &fs, const char *fname) {

  Serial.println("start read file");
  file = fs.open(fname);
  if (!file) {
    return false;
  }
  renderer->reset();
  Serial.printf("ILDA_Header_t size= %d \n", sizeof(ILDA_Header_t));
  file.read((uint8_t *)&header, sizeof(ILDA_Header_t));
  Serial.printf("frame_name = %s", header.frame_name);
  header.records = ntohs(header.records);
  header.total_frames = ntohs(header.total_frames);
  dump_header(header);
  file_frames = header.total_frames;
  frameStart = file.position();
  Serial.printf("file_frames=%d\n",file_frames);
  Serial.println("end read file");
  cur_buffer = 0;
  return true;
}

bool ILDAFile::tickNextFrame() {
  // Serial.println("175");
  // Serial.print("cur_buffer = ");
  // Serial.println(cur_buffer);
  // Serial.print("frames[cur_buffer].isBuffered = ");
  // Serial.println(frames[cur_buffer].isBuffered);
  // Serial.print("companyname = ");
  // Serial.println(header.company_name);
  // Serial.print("header.records = ");
  // Serial.println(header.records);
  if (frames[cur_buffer].isBuffered == false) {
    frames[cur_buffer].isBuffered = true;
    frames[cur_buffer].number_records = header.records;
    frames[cur_buffer].duration = header.reserved2;
    //frames[cur_buffer].records = (ILDA_Record_t *)malloc(sizeof(ILDA_Record_t) * header.records);
    ILDA_Record_t *records = frames[cur_buffer].records;
    for (int i = 0; i < header.records; i++) {
      file.read((uint8_t *)(records + i), sizeof(ILDA_Record_t));
      // Serial.printf("x,y,z,statusCode,color = %d,%d,%d,%d,%d\n",records[i].x,records[i].y,records[i].z,records[i].status_code,records[i].color);
      records[i].x = ntohs(records[i].x);
      records[i].y = ntohs(records[i].y);
      records[i].z = ntohs(records[i].z);
    }
    // frames[cur_buffer].isBuffered = true;
    cur_buffer++;
    if (cur_buffer > bufferFrames - 1) {
      cur_buffer = 0;
    }
    if (cur_frame < file_frames) {
      // read the next header
      // file.read((uint8_t *)&header, sizeof(ILDA_Header_t));
      // Serial.printf("cur_frame_name = %s\n", header.frame_name);
      // header.records = ntohs(header.records);
      // header.total_frames = ntohs(header.total_frames);
      cur_frame++;
    }
    else{
      file.seek(0);
      cur_frame = 0;
    }
    file.read((uint8_t *)&header, sizeof(ILDA_Header_t));
    Serial.printf("cur_frame_name = %s\n", header.frame_name);
    header.records = ntohs(header.records);
    header.total_frames = ntohs(header.total_frames);
    Serial.printf("263 cur_frame = %d\n",cur_frame);
    // if (cur_frame > file_frames - 1) {
    //   cur_frame = 0;
    //   // cur_frame_render = 0;
    //   if (digitalRead(4) == HIGH) {  //Happy按钮，自动下一个
    //     nextMedia(1);
    //   } else {
    //     Serial.println("next");        
    //     //暂时不自动播放下一个
    //     //nextMedia(0);
    //   }
    // }
    if (file_frames > 0) {
      progressNum = ((float)cur_frame / (float)file_frames) * 9 + 0.5;
    } else progressNum = 9;
    return true;
  } else return false;  //This frame has been buffered and not display yet.. 该帧已缓存且未Render，可能是读文件、串流太快了？忽视掉就好 0w0
}

#define bufferLen 6
int loadedLen = 0;
bool ILDAFile::parseStream(uint8_t *data, size_t len, int frameIndex, int totalLen) {
  if (frames[cur_buffer].isBuffered == false) {
    //frames[cur_buffer].isBuffered = true;
    frames[cur_buffer].number_records = totalLen / bufferLen;
    ILDA_Record_t *records = frames[cur_buffer].records;

    /*
      Serial.print("Len: ");
      Serial.println(len);
      Serial.print("Get Frame: ");
      
      Serial.print(frameIndex);
      Serial.print(" / ");
      Serial.print(totalLen);
      Serial.print("  ");
      Serial.println(cur_buffer);
      */

    for (size_t i = 0; i < len / bufferLen; i++) {
      int16_t x = (data[i * bufferLen] << 8) | data[i * bufferLen + 1];
      int16_t y = (data[i * bufferLen + 2] << 8) | data[i * bufferLen + 3];

      /*
        Serial.print(frameIndex/bufferLen + i);
        Serial.print(",");
        Serial.print(x);
        Serial.print(",");
        Serial.println(y);
        */
      /*
        Serial.print(",");
        Serial.print(data[i*bufferLen+4]);
        Serial.print(",");
        Serial.println(data[i*bufferLen+5]);
        Serial.println((data[i*bufferLen+5] & 0b01000000) == 0);*/


      records[frameIndex / bufferLen + i].x = x;
      records[frameIndex / bufferLen + i].y = y;
      records[frameIndex / bufferLen + i].z = 0;
      records[frameIndex / bufferLen + i].color = data[i * bufferLen + 4];
      records[frameIndex / bufferLen + i].status_code = data[i * bufferLen + 5];
    }
    loadedLen += len;

    if (loadedLen >= totalLen) {
      //Serial.println("Frame End");
      loadedLen = 0;
      cur_buffer++;
      if (cur_buffer > bufferFrames - 1) cur_buffer = 0;

      cur_frame++;
      //Serial.println(cur_frame);
      if (cur_frame > file_frames - 1) {
        cur_frame = 0;
      }
    }

    return true;
  } else return false;  //This frame has been buffered and not display yet.. 该帧已缓存且未Render，可能是读文件、串流太快了？忽视掉就好 0w0
}



void draw_task() {
  if(renderer != NULL){
    renderer->draw();
  }
}

void IRAM_ATTR SPIRenderer::draw() {
  // Clear the interrupt
  // do we still have things to draw?
  // Serial.println("start draw");
  // Serial.println(ilda->frames[frame_position].number_records);
  // printf("draw_position=%d , number_records=%d \n",draw_position,ilda->frames[frame_position].number_records);
  static unsigned long frame_durate_time = millis();
  if (isLoading == true){
    return;
  }
  // Serial.println("358");
  // Serial.println(draw_position);
  // Serial.println(ilda->frames[frame_position].number_records);
  if (draw_position < ilda->frames[frame_position].number_records) {
    const ILDA_Record_t &instruction = ilda->frames[frame_position].records[draw_position];
    // Serial.print("x,y=");
    // Serial.print(instruction.x);
    // Serial.print(" ");
    // Serial.println(instruction.y);
    int y = 2048 + ((int)instruction.x * 1024) / 16384;
    int x = 2048 + ((int)instruction.y * 1024) / 16384;

    // Serial.print("x,y=");
    // Serial.print(instruction.x);
    // Serial.print(" ");
    // Serial.println(instruction.y);
    // Serial.print("x,y=");
    // Serial.print(x);
    // Serial.print(" ");
    // Serial.println(y);
    // set the laser state

    // channel A
    spi_transaction_t t1 = {};
    t1.length = 16;
    t1.flags = SPI_TRANS_USE_TXDATA;
    t1.tx_data[0] = 0b11010000 | ((x >> 8) & 0xF);
    t1.tx_data[1] = x & 255;
    spi_device_polling_transmit(spi, &t1);
    // channel B
    spi_transaction_t t2 = {};
    t2.length = 16;
    t2.flags = SPI_TRANS_USE_TXDATA;
    t2.tx_data[0] = 0b01010000 | ((y >> 8) & 0xF);
    t2.tx_data[1] = y & 255;
    spi_device_polling_transmit(spi, &t2);


    // Serial.println("358");
    //把激光开启↓
    // Serial.printf("s,c = %d,%d\n",instruction.status_code,instruction.color);
    // Serial.printf("and = %d\n",instruction.status_code & 0b01000000);
    if ((instruction.status_code & 0b01000000) == 0) {
      if (instruction.color <= 9) {  //RED
        digitalWrite(PIN_NUM_LASER_R, LOW);
      } else if (instruction.color <= 18) {  //YELLOW
        digitalWrite(PIN_NUM_LASER_R, LOW);
        digitalWrite(PIN_NUM_LASER_G, LOW);
      } else if (instruction.color <= 27) {  //GREEN
        digitalWrite(PIN_NUM_LASER_G, LOW);
      } else if (instruction.color <= 36) {  //CYAN
        digitalWrite(PIN_NUM_LASER_G, LOW);
        digitalWrite(PIN_NUM_LASER_B, LOW);
      } else if (instruction.color <= 45) {  //BLUE
        digitalWrite(PIN_NUM_LASER_B, LOW);
      } else if (instruction.color <= 54) {  //Magenta
        digitalWrite(PIN_NUM_LASER_B, LOW);
        digitalWrite(PIN_NUM_LASER_R, LOW);
      } else if (instruction.color <= 63) {  //WHITE
        digitalWrite(PIN_NUM_LASER_B, LOW);
        digitalWrite(PIN_NUM_LASER_R, LOW);
        digitalWrite(PIN_NUM_LASER_G, LOW);
      }
    } else {  //不亮的Point
      digitalWrite(PIN_NUM_LASER_R, HIGH);
      digitalWrite(PIN_NUM_LASER_G, HIGH);
      digitalWrite(PIN_NUM_LASER_B, HIGH);
    }
    // Serial.println("394");
    // DAC Load
    digitalWrite(PIN_NUM_LDAC, LOW);
    digitalWrite(PIN_NUM_LDAC, HIGH);

    draw_position++;
  } else {
    // Serial.println("402");
    unsigned long curmillis = millis();
    draw_position = 0;
    if(ilda->frames[frame_position].number_records>0){
      // Serial.printf("reserved2= %d, projector_number= %d\n",header.reserved2,header.projector_number);
      // Serial.printf("curmillis = %d, frame_durate_time= %d, reserved2= %d\n",curmillis,frame_durate_time,header.reserved2*1000);
    }
    if(curmillis- frame_durate_time >ilda->frames[frame_position].duration * 1000){
      frame_durate_time = curmillis;
      if (ilda->file_frames != 1){
        ilda->frames[frame_position].isBuffered = false;
        frame_position++;
        ilda->cur_frame_render++;
      }
      // ilda->cur_frame++;
      Serial.printf("fp,ff = %d,%d\n",frame_position,ilda->file_frames);
      Serial.printf("cur_buffer = %d\n",ilda->cur_buffer);

      if (frame_position >= bufferFrames || frame_position >= ilda->file_frames) {
        frame_position = 0;
      }
      if (!isStreaming) {
        xTaskNotifyGive(fileBufferHandle);
      }
      // Serial.println("414");
    }
    else{

    }
  }
}

SPIRenderer::SPIRenderer() {
  frame_position = 0;
  draw_position = 0;
}

void SPIRenderer::reset(){
  frame_position = 0;
  draw_position = 0;  
}

void SPIRenderer::start() {

  pinMode(PIN_NUM_LASER_R, OUTPUT);
  pinMode(PIN_NUM_LASER_G, OUTPUT);
  pinMode(PIN_NUM_LASER_B, OUTPUT);
  pinMode(PIN_NUM_LDAC, OUTPUT);

  // setup SPI output
  esp_err_t ret;
  spi_bus_config_t buscfg = {
    .mosi_io_num = PIN_NUM_MOSI,
    .miso_io_num = PIN_NUM_MISO,
    .sclk_io_num = PIN_NUM_CLK,
    .quadwp_io_num = -1,
    .quadhd_io_num = -1,
    .max_transfer_sz = 0
  };
  spi_device_interface_config_t devcfg = {
    .command_bits = 0,
    .address_bits = 0,
    .dummy_bits = 0,
    .mode = 0,
    .clock_speed_hz = 40000000,
    .spics_io_num = PIN_NUM_CS,  //CS pin
    .flags = SPI_DEVICE_NO_DUMMY,
    .queue_size = 2,
  };
  //Initialize the SPI bus
  ret = spi_bus_initialize(HSPI_HOST, &buscfg, 1);
  printf("Ret code is %d\n", ret);
  ret = spi_bus_add_device(HSPI_HOST, &devcfg, &spi);
  printf("Ret code is %d\n", ret);

  xTaskCreatePinnedToCore(
    fileBufferLoop, "fileBufferHandle", 5000  // Stack size
    ,
    NULL, 3  // Priority
    ,
    &fileBufferHandle, 0);
}


//Current ILDA Buffer  当前的ILDA内存，采用Buffer的形式，为了能更快的加载大型ILDA文件。动态读取文件，申请内存，避免一下子把整个ILDA文件的所有帧的内存都申请了（没有那么多PSRAM）
uint8_t chunkTemp[64];
int tempLen = 0;

void setupRenderer() {
  Serial.print("RAM Before:");
  Serial.println(ESP.getFreeHeap());
  ilda->frames = (ILDA_Frame_t *)malloc(sizeof(ILDA_Frame_t) * bufferFrames);
  for (int i = 0; i < bufferFrames; i++) {
    ilda->frames[i].isBuffered = false;
    ilda->frames[i].number_records = 0;
    ilda->frames[i].records = (ILDA_Record_t *)malloc(sizeof(ILDA_Record_t) * MAXRECORDS);
  }
  Serial.print("RAM After:");
  Serial.println(ESP.getFreeHeap());
  renderer = new SPIRenderer();
  nextMedia(1);
  renderer->start();
  Serial.println("setupRenderer end:");
}


void handleStream(uint8_t *data, size_t len, int index, int totalLen) {
  //Serial.println("Stream");
  int newtempLen = (tempLen + len) % 6;
  //Serial.print("newTemp:");
  //Serial.println(newtempLen);
  if (tempLen > 0) {
    //memcpy(chunkTemp+tempLen, data, len - newtempLen);
    uint8_t concatData[len - newtempLen + tempLen];
    memcpy(concatData, chunkTemp, tempLen);
    memcpy(concatData + tempLen, data, len - newtempLen);  // copy the address
    //Serial.print("Temp Concat Len: ");
    //Serial.println(len-newtempLen+tempLen);
    ilda->parseStream(concatData, len - newtempLen + tempLen, index - tempLen, totalLen);
  } else {
    //Serial.print("No Concat Len: ");
    //Serial.println(len-newtempLen+tempLen);
    ilda->parseStream(data, len - newtempLen, index, totalLen);
  }
  for (size_t i = 0; i < newtempLen; i++) {
    chunkTemp[i] = data[len - newtempLen + i];
  }
  tempLen = newtempLen;
}

void nextMedia(int position) {
  if (position == 1) {
    curMedia++;
  } else if (position == -1) {
    curMedia--;
  } else {
    curMedia = curMedia + position;
  }
  if (curMedia >= avaliableMedia.size()) curMedia = 0;
  if (curMedia < 0) curMedia = avaliableMedia.size() - 1;
  Serial.printf("avaliableMedia.size() = %d\n",avaliableMedia.size());
  String filePath = String("/bbLaser/") += avaliableMedia[curMedia].as<String>();
  Serial.printf("filePath = %s\n",filePath);
  ilda->cur_frame = 0;
  ilda->cur_frame_render = 0;
  ilda->read(SD, filePath.c_str());
}


//=============  LED  -_,- ================//
// uint32_t Wheel(byte WheelPos) {
//   WheelPos = 255 - WheelPos;
//   if(WheelPos < 85) {
//     return strip.Color(255 - WheelPos * 3, 0, WheelPos * 3);
//   }
//   if(WheelPos < 170) {
//     WheelPos -= 85;
//     return strip.Color(0, WheelPos * 3, 255 - WheelPos * 3);
//   }
//   WheelPos -= 170;
//   return strip.Color(WheelPos * 3, 255 - WheelPos * 3, 0);
// }

// void rainbow(uint8_t wait) {
//   if(pixelInterval != wait)
//     pixelInterval = wait;
//   for(uint16_t i=0; i < pixelNumber; i++) {
//     if(i < progressNum){
//       strip.setPixelColor(pixelNumber - i, Wheel((i * 4 + pixelCycle ) & 255)); //  Update delay time
//     }
//     else strip.setPixelColor(pixelNumber - i,strip.Color(0, 0, 0));
//   }
//   strip.show();                             //  Update strip to match
//   pixelCycle+=4;                             //  Advance current cycle
//   if(pixelCycle >= 256)
//     pixelCycle = 0;                         //  Loop the cycle back to the begining
// }

//====================================//

//  Core 2 //
unsigned long timeDog = 0;
void fileBufferLoop(void *pvParameters) {
  for (;;) {
    if (millis() - timeDog > 1000) {
      timeDog = millis();
      TIMERG0.wdt_wprotect = TIMG_WDT_WKEY_VALUE;
      TIMERG0.wdt_feed = 1;
      TIMERG0.wdt_wprotect = 0;
    }
    if (!isStreaming && isWriting == false) {
      if (buttonState == 1) {
        nextMedia(-1);
        buttonState = 0;
      } else if (buttonState == 2) {
        nextMedia(1);
        buttonState = 0;
      }
      if (!ilda->tickNextFrame()) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
      }
    }
  }
}

volatile unsigned long serial_time_old = 0;
// volatile unsigned long serial_time_receive=0;

void handleSerialData() {
  // size_t bufferSize = 2048;
  // uint8_t readBuffer[bufferSize];
  if (micros() - serial_time_old > 1000000 && isWriting == false) {
    Serial.println("waiting serial");
    serial_time_old = micros();
  }
  if (isLoading && isWriting == false) {
    file.close();
    tmpFile = SD.open(TMP_FILE, FILE_WRITE);
    isWriting = true;
    writeLength = 0;
    Serial.println("Start Writing!");
  }
  if (!isLoading && isWriting && serialBufferReadFlag) {
    tmpFile.close();
    Serial.println("tmpFile closed!");
    Serial.println("Writing finished!");
    isWriting = false;
    for (int i = 0; i < bufferFrames; i++) {
      ilda->frames[i].isBuffered = false;
      ilda->frames[i].number_records = 0;
    }
    ilda->cur_buffer = 0;
    ilda->cur_frame = 0;
    ilda->cur_frame_render = 0;
    ilda->read(SD, TMP_FILE);
  }
  uint8_t serialFileBuffer[2048];
  while (!serialBufferReadFlag) {
    if (xSemaphoreTake(shared_serial_mutex, portMAX_DELAY) == pdTRUE) {
      uint serialFileBufferSize = 0;
      uint readLen = serialBufferLen;
      memcpy(serialFileBuffer, serialBuffer, serialBufferLen);
      serialBufferReadFlag = true;
      xSemaphoreGive(shared_serial_mutex);
      tmpFile.write(serialFileBuffer, readLen);
      writeLength += readLen;
      Serial.print("writeLength: ");
      Serial.println(writeLength);
    }
    // if(micros()-serial_time_receive <= 100000){
    //   continue;
    // }
    // serial_time_receive = micros();

    // if(isLoading==false){
    //   uint8_t head[2];
    //   Serial.read(head,(size_t)2);
    //   if(0xAA == head[0] && 0xBB == head[1]){
    //     isLoading = true;
    //     file.close();
    //     Serial.println("cur file closed!");
    //     Serial.read(fileLength,(size_t)4);
    //     fileLength_int = *((uint *) fileLength);
    //     printf("bytes = %d %d %d %d\n", fileLength[0],fileLength[1],fileLength[2],fileLength[3]);
    //     Serial.println("start loading");
    //     printf("fileLength = %d\n",fileLength_int);
    //     readLength = 0;
    //     tmpFile =SD.open(TMP_FILE, FILE_WRITE);
    //   }
    // }
    // else{
    //   if(isLoading){
    //     if(readLength < fileLength_int){
    //       // printf("available = %d\n",Serial.available());
    //       size_t readSize = Serial.read(readBuffer, bufferSize);
    //       tmpFile.write(readBuffer , readSize);
    //       readLength += readSize;
    //       Serial.print("readLength: ");
    //       Serial.println(readLength);
    //     }
    //     if(readLength>= fileLength_int){
    //       tmpFile.close();
    //       Serial.println("tmpFile closed!");
    //       Serial.println("loading finished!");
    //       isLoading = false;
    //       ilda->cur_frame = 0;
    //       ilda->read(SD,TMP_FILE);
    //     }
    //   }
    // }
  }
}

void serialReadLoop(void *pvParameters) {
  size_t bufferSize = 2048;
  while (true) {
    // Serial.println(Serial.available());
    if (Serial.available() > 0) {
      if (isLoading == false) {
        uint8_t head[2];
        Serial.read(head, (size_t)2);
        if (0xAA == head[0] && 0xBB == head[1]) {
          isLoading = true;
          Serial.read(fileLength, (size_t)4);
          fileLength_int = *((uint *)fileLength);
          printf("bytes = %d %d %d %d\n", fileLength[0], fileLength[1], fileLength[2], fileLength[3]);
          printf("fileLength = %d\n", fileLength_int);
          readLength = 0;
        }
      }
      while (readLength < fileLength_int) {
        if (Serial.available() > 0) {
          // printf("available = %d\n",Serial.available());
          if (xSemaphoreTake(shared_serial_mutex, portMAX_DELAY) == pdTRUE) {
            if (serialBufferReadFlag) {
              serialBufferLen = 0;
              serialBufferReadFlag = false;
            }
            size_t readSize = Serial.read(serialBuffer + serialBufferLen, bufferSize);
            readLength += readSize;
            serialBufferLen += readSize;
            xSemaphoreGive(shared_serial_mutex);
            // Serial.print("readLength: ");
            // Serial.println(readLength);
          }
        }
      }
      Serial.print("readLength: ");
      Serial.println(readLength);
      isLoading = false;
    }
    vTaskDelay(10);
  }
}


// unsigned long timeDog2 = 0;
// void ledLoop(void *pvParameters){
//   for (;;){
//     unsigned long currentMillis = millis();
//     if(currentMillis - timeDog2 > 1000){
//       timeDog2 = currentMillis;
//       TIMERG0.wdt_wprotect=TIMG_WDT_WKEY_VALUE;
//       TIMERG0.wdt_feed=1;
//       TIMERG0.wdt_wprotect=0;
//     }
//     if(currentMillis - pixelPrevious >= pixelInterval) {        //  Check for expired time
//       pixelPrevious = currentMillis;                            //  Run current frame
//       if(!isStreaming){
//         rainbow(30); // Flowing rainbow cycle along the whole strip
//       }
//       else{
//         for(uint16_t i=0; i < pixelNumber; i++) {
//             strip.setPixelColor(i,strip.Color(0, 255, 0)); //  Update delay time
//         }
//         strip.show();
//       }
//     }
//   }
// }
