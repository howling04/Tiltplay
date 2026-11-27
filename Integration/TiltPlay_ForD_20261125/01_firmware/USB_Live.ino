// Temporary USB raw logger for C's offline six-axis data.
// Native USB CDC transports one complete JSON object per line.
// This sketch intentionally records locally and has no network credentials.
#include <Arduino.h>
#include <Arduino_LSM6DSOX.h>
#include <mbed.h>
#include <kvstore_global_api.h>
#include <math.h>
#include <ctype.h>
#include <errno.h>
static const char DEVICE_ID[] = "nano-a1b2c3d4e5f6";
static const uint32_t linkEpoch=1;
uint32_t lowPrevious=0, sampleSeq=0, publishedSeq=0, usbDrops=0;
uint64_t timeExtended=0;
char BOOT_ID[17];
uint32_t lastStatusMs=0;
char command[384];
size_t commandLength=0;
bool commandOverflow=false;
uint64_t clockUs() {
  uint32_t low=micros();
  timeExtended+=uint32_t(low-lowPrevious);
  lowPrevious=low;
  return timeExtended;
}
void skipSpaces(const char *&cursor) {
  while (isspace(static_cast<unsigned char>(*cursor))) ++cursor;
}

bool take(const char *&cursor, char expected) {
  skipSpaces(cursor);
  if (*cursor != expected) return false;
  ++cursor;
  return true;
}

bool readProbeString(const char *&cursor, char *out, size_t capacity) {
  if (!take(cursor, '"')) return false;
  size_t length = 0;
  while (*cursor && *cursor != '"') {
    unsigned char ch = *cursor++;
    if (!(isalnum(ch) || ch == '-') || length + 1 >= capacity) return false;
    out[length++] = ch;
  }
  if (*cursor != '"') return false;
  ++cursor;
  out[length] = '\0';
  return length != 0;
}

bool parsePing(const char *cursor, char (&cmdId)[65]) {
  static const char *const keys[] = {
    "schema", "device_id", "boot_id", "link_epoch",
    "cmd_id", "op", "args", "deadline_t_us"
  };
  unsigned seen = 0;
  if (!take(cursor, '{')) return false;
  do {
    char key[24];
    // Keys can contain underscores. They are fixed ASCII names in B's JSON.
    if (!take(cursor, '"')) return false;
    size_t length = strspn(cursor, "abcdefghijklmnopqrstuvwxyz_");
    if (!length || length >= sizeof(key) || cursor[length] != '"') return false;
    memcpy(key, cursor, length);
    key[length] = '\0';
    cursor += length + 1;
    if (!take(cursor, ':')) return false;
    unsigned field = 0;
    while (field < 8 && strcmp(key, keys[field])) ++field;
    if (field == 8 || (seen & (1u << field))) return false;
    seen |= 1u << field;
    if (field == 6) {
      if (!take(cursor, '{') || !take(cursor, '}')) return false;
    } else if (field == 0 || field == 3 || field == 7) {
      skipSpaces(cursor);
      if (*cursor < '0' || *cursor > '9') return false;
      if (*cursor == '0' && isdigit(static_cast<unsigned char>(cursor[1]))) return false;
      char *end = nullptr;
      errno = 0;
      unsigned long long number = strtoull(cursor, &end, 10);
      if (errno || (field == 0 && number != 2) ||
          (field == 3 && number != linkEpoch)) return false;
      // B can send a boot-relative deadline of 500000 before it has raw.
      cursor = end;
    } else {
      char value[65];
      if (!readProbeString(cursor, value, sizeof(value))) return false;
      if (field == 1 && strcmp(value, DEVICE_ID)) return false;
      if (field == 2 && strcmp(value, BOOT_ID)) return false;
      if (field == 5 && strcmp(value, "ping")) return false;
      if (field == 4) strcpy(cmdId, value);
    }
  } while (take(cursor, ','));
  if (!take(cursor, '}')) return false;
  skipSpaces(cursor);
  return seen == 0xff && !*cursor;
}


void emitStatus() {
  char payload[256];
  snprintf(payload,sizeof(payload),
    "{\"channel\":\"status\",\"message\":{\"schema\":2,\"device_id\":\"nano-a1b2c3d4e5f6\",\"boot_id\":\"%s\",\"link_epoch\":1,\"online\":true,\"capabilities\":[\"raw6\"]}}",BOOT_ID);
  Serial.println(payload);
}
void serviceCommands() {
  while(Serial.available()) {
    int value=Serial.read();
    if(value<0)break;
    char c=char(value);
    if(c=='\n') {
      if(!commandOverflow && commandLength) {
        command[commandLength]=0;
        char id[65];
        if(parsePing(command,id)) {
          uint64_t now=clockUs();
          char reply[320];
          snprintf(reply,sizeof(reply),
            "{\"channel\":\"ack\",\"message\":{\"schema\":2,\"device_id\":\"nano-a1b2c3d4e5f6\",\"boot_id\":\"%s\",\"link_epoch\":1,\"cmd_id\":\"%s\",\"ok\":true,\"code\":\"PONG\",\"t_us\":%llu}}",
            BOOT_ID,id,(unsigned long long)now);
          Serial.println(reply);
        }
      }
      commandLength=0;commandOverflow=false;
    } else if(c!='\r') {
      if(commandLength+1<sizeof(command))command[commandLength++]=c;
      else commandOverflow=true;
    }
  }
}
void setup() {
  Serial.begin(921600);
  // Start sensor only after the receiving application opens the USB port.
  while (!Serial) delay(10);
  uint64_t count=1;
  size_t size=0;
  int result=kv_get("/kv/tiltplay_boot",&count,sizeof(count),&size);
  if ((result!=MBED_SUCCESS && result!=MBED_ERROR_ITEM_NOT_FOUND) ||
      (result==MBED_SUCCESS && size!=sizeof(count)) || count==UINT64_MAX) {
    Serial.println("{\"error\":\"boot_store_read\"}"); while(true)delay(1000);
  }
  ++count;
  if(kv_set("/kv/tiltplay_boot",&count,sizeof(count),0)!=MBED_SUCCESS) {
    Serial.println("{\"error\":\"boot_store_write\"}"); while(true)delay(1000);
  }
  snprintf(BOOT_ID,sizeof(BOOT_ID),"%016llx",(unsigned long long)count);
  if(!IMU.begin()) {
    Serial.println("{\"error\":\"imu_begin\"}"); while(true)delay(1000);
  }
  emitStatus();
}
void loop() {
  clockUs();
  serviceCommands();
  if (millis()-lastStatusMs>=1000) {emitStatus();lastStatusMs=millis();}
  if (!IMU.accelerationAvailable() || !IMU.gyroscopeAvailable()) {delay(1);return;}
  float ax,ay,az,gx,gy,gz;
  if(!IMU.readAcceleration(ax,ay,az) || !IMU.readGyroscope(gx,gy,gz))return;
  ++sampleSeq;
  if(!isfinite(ax)||!isfinite(ay)||!isfinite(az)||!isfinite(gx)||!isfinite(gy)||!isfinite(gz))return;
  uint64_t timestamp=clockUs();
  const float gravity=9.80665f, toRadians=0.017453292519943295f;
  char line[512];
  int n=snprintf(line,sizeof(line),
    "{\"schema\":2,\"device_id\":\"nano-a1b2c3d4e5f6\",\"boot_id\":\"%s\","
    "\"link_epoch\":1,\"seq\":%lu,\"sample_seq\":%lu,\"t_us\":%llu,"
    "\"acc_mps2\":[%.5f,%.5f,%.5f],\"gyro_rps\":[%.6f,%.6f,%.6f],"
    "\"button\":{\"jump\":false,\"press_count\":0,\"press_t_us\":null},\"sample_hz\":104}",
    BOOT_ID,(unsigned long)(publishedSeq+1),(unsigned long)sampleSeq,
    (unsigned long long)timestamp,ax*gravity,ay*gravity,az*gravity,
    gx*toRadians,gy*toRadians,gz*toRadians);
  if(n<=0 || n>=int(sizeof(line))) {++usbDrops;return;}
  if(!Serial) {++usbDrops;return;}
  Serial.print("{\"channel\":\"raw\",\"message\":");
  Serial.print(line);
  Serial.println("}");
  ++publishedSeq;
}

