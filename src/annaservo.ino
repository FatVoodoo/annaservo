#include <ESP8266WiFi.h>
#include <Servo.h>
#include "step.h"
#include "secrets.h"
extern "C"{
#include "spi_flash.h"
}
extern "C" uint32_t _SPIFFS_end;

#define WEBAPP 0  // use 1 to enable web application serving
#if WEBAPP
#include "webapp.h"
#endif

Servo servos[SERVO_COUNT];
int otaMode = 0;
const int MAX_STEPS = 4090 / sizeof(Step);
const uint32_t _sector = ((uint32_t)&_SPIFFS_end - 0x40200000) / SPI_FLASH_SEC_SIZE;
WiFiServer server(80);
int runMode = false;
int nextStep = 0;


Servo myservo;

int pos = 0;    // variable to store the servo position

// handles one program step to a set of positions, with timed moves
Step::Step() {
      stepTime = 0;
      for (int i = 0; i < SERVO_COUNT; i++) {
        pos[i] = 90;
      }
    }
void Step::moveTo() {
  if (stepTime == 0) {
    // no time - go directly to destination
    for (int i = 0; i < SERVO_COUNT; i++) {
      servos[i].write(pos[i]);
    }
  } else {
    int tickCount = 100L * stepTime / SERVO_TICK_DELAY_MS;
    struct posDist {
      float currPos;
      float tickDist;
    } posDists[SERVO_COUNT];

    for (int i = 0; i < SERVO_COUNT; i++) {
      struct posDist &posDist = posDists[i];
      posDist.currPos = (float)servos[i].read();
      posDist.tickDist = (pos[i] - posDist.currPos) / tickCount;
    }
    while (tickCount-- > 0) {
      for (int i = 0; i < SERVO_COUNT; i++) {
        struct posDist &posDist = posDists[i];
        posDist.currPos += posDist.tickDist;
        servos[i].write((int)posDist.currPos);
      }
      delay(SERVO_TICK_DELAY_MS);
    }
  }
}

const long programMagicNumber = 671349586L; // used to check if flash data is a program saved by this app
struct {
  long magicNumber;
  int formatVersion;
  Step steps[MAX_STEPS];
  int stepCount;
} program __attribute__((aligned(4)));

int saveProgram() {
  int success = 0;
  program.magicNumber = programMagicNumber;
  program.formatVersion = 1;
  noInterrupts();
  if(spi_flash_erase_sector(_sector) == SPI_FLASH_RESULT_OK) {
    if(spi_flash_write(_sector * SPI_FLASH_SEC_SIZE, reinterpret_cast<uint32_t*>(&program), sizeof(program)) == SPI_FLASH_RESULT_OK) {
      success = 1;
    }
  }
  interrupts();
  return success;
}

int restoreProgram() {
  int success = 0;
  noInterrupts();
  if(spi_flash_read(_sector * SPI_FLASH_SEC_SIZE, reinterpret_cast<uint32_t*>(&program), sizeof(program)) == SPI_FLASH_RESULT_OK) {
    if (program.magicNumber == programMagicNumber) {
      success = 1;
    } else {
      // flash did not contain a valid program - clear it
      Serial.println("restoreProgram: not valid program in flash");
      program.stepCount = 0;
      program.formatVersion = 1;
    }
  }
  interrupts();
  return success;
}

void attachServos() {
  servos[0].attach(16);
  servos[1].attach(14);
  servos[2].attach(12);
  servos[3].attach(13);
  servos[4].attach(15);
  servos[5].attach(4);
}

void detachServos() {
  servos[0].detach();
  servos[1].detach();
  servos[2].detach();
  servos[3].detach();
  servos[4].detach();
  servos[5].detach();
}

void httpRespond(WiFiClient client, int status) {
  client.print("HTTP/1.1 ");
  client.print(status);
  client.println(" OK");
  client.println(""); // mark end of headers
}

void httpRespond(WiFiClient client, int status, const char *contentType) {
  client.print("HTTP/1.1 ");
  client.print(status);
  client.println(" OK");
  client.print("Content-Type: "); client.println(contentType);
  client.println(""); // mark end of headers
}

#if WEBAPP
bool loadFromFlash(WiFiClient &client, String path) {
  if (path.endsWith("/")) path += "index.html";
  int NumFiles = sizeof(files)/sizeof(struct t_websitefiles);
  for (int i=0; i<NumFiles; i++) {
    if (path.endsWith(String(files[i].filename))) {
      client.println("HTTP/1.1 200 OK");
      client.print("Content-Type: "); client.println(files[i].mime);
      client.print("Content-Length"); client.println(String(files[i].len));
      client.println(""); //  do not forget this one
      _FLASH_ARRAY<uint8_t>* filecontent = (_FLASH_ARRAY<uint8_t>*)files[i].content;
      filecontent->open();
      client.write(*filecontent, 100);
      return true;
    }
  }
  httpRespond(client, 201);
  return false;
}
#endif

// convert string representation into referenced Step
// s is t,p,p,p,p,p,p,p
// omittes numbers are not set
void stringToStep(String s, Step &step) {
  int i = 0, j;
  j = s.indexOf(',', i);
  if (i < j) {
    float t = s.substring(i, j).toFloat();
    step.stepTime = (int)(t * 10 + 0.5);
  }
  i = j + 1;
  for (int posi = 0; posi < SERVO_COUNT; posi++) {
    if (j < s.length()) {
      j = s.indexOf(',', i);
      if (j == -1) {
        j = s.length();
      }
      if (i < j) {
        step.pos[posi] = s.substring(i, j).toInt();
      }
      i = j + 1;
    }
  }
}

void printStepsJson(WiFiClient client) {
  client.println("[");
  for (int i = 0; i < program.stepCount; i++) {
    client.print("  {\"timeToStep\": ");
    int t = program.steps[i].stepTime;
    client.print(t / 10); client.print("."); client.print(t % 10);
    client.print(", \"positions\": [");
    for (int j = 0; j < SERVO_COUNT; j++) {
      if (j > 0) {
        client.print(", ");
      }
      client.print(program.steps[i].pos[j]);
    }
    client.print("]}");
    if (i < program.stepCount - 1) {
      client.print(",");
    }
    client.println("");
  }
  client.println("]");
}

String getRequestQuery(String s) {
  int i = s.indexOf(' ');
  int j = s.lastIndexOf(' ');
  return s.substring(i + 1, j);
}

void runProgram() {
  while (1) {
    for (int i = 0; i < program.stepCount; i++) {
      program.steps[i].moveTo();
    }
  }
}

int parseIntUntil(String s, int &intResult, int &startI) {
  return parseIntUntil(s, intResult, startI, 0);
}

int parseIntUntil(String s, int &intResult, int &startI, char endChar) {
  int i = endChar == 0 ? s.length() : s.indexOf(endChar, startI);
  if (i != -1) {
    intResult = s.substring(startI, i).toInt();
    startI = i + 1;
    return 1;
  }
  return 0;
}

int parseStringToEnd(String s, String &stringResult, int &startI) {
  stringResult = s.substring(startI);
  startI = s.length();
  return 1;
}

void setup() {
  program.stepCount = 0;

  Serial.begin(115200);
  Serial.println("\nBooting");

  WiFi.mode(WIFI_STA);    // guarantee gap free movements
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("Ready");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  attachServos();
//  myservo.attach(16);  // attaches the servo to a GPIO pin
}

void loop() {
  // for (pos = 0; pos <= 180; pos += 1) { // goes from 0 degrees to 180 degrees
  //   // in steps of 1 degree
  //   myservo.write(pos);              // tell servo to go to position in variable 'pos'
  //   delay(15);                       // waits 15ms for the servo to reach the position
  // }
  // for (pos = 180; pos >= 0; pos -= 1) { // goes from 180 degrees to 0 degrees
  //   myservo.write(pos);              // tell servo to go to position in variable 'pos'
  //   delay(15);                       // waits 15ms for the servo to reach the position
  // }

  //Check if a client has connected
  WiFiClient client = server.available();
  if (!client) {
    return;
  }


  // Wait until the client sends some data
  while(!client.available()){
    delay(1);
  }

  // Read the first line of the request
  String request = client.readStringUntil('\r');
  Serial.println(request);
  client.flush();

  // Match the request
  String query = getRequestQuery(request);
  if (query.equals("/save")) {
    if (saveProgram()) {
      httpRespond(client, 200);
    } else {
      httpRespond(client, 500);
    }
  } else if (query.equals("/restore")) {
    if (restoreProgram()) {
      httpRespond(client, 200);
    } else {
      httpRespond(client, 500);
    }
  } else if (query.equals("/stepCount")) {
    httpRespond(client, 200, "application/json");
    client.println(program.stepCount);
  } else if (query.equals("/steps")) {
    httpRespond(client, 200, "application/json");
    printStepsJson(client);
  } else if (query.startsWith("/add/")) {
    int stepi;
    String stepString;
    int i = 5;
    if (parseIntUntil(query, stepi, i, '/')
        && parseStringToEnd(query, stepString, i)) {
      if (0 <= stepi && stepi <= program.stepCount) {
        if (stepi < program.stepCount) {
          for (int i = program.stepCount; i > stepi; i--) {
            program.steps[i] = program.steps[i - 1];
          }
        }
        program.stepCount++;
        stringToStep(stepString, program.steps[stepi]);
        httpRespond(client, 200);
        return;
      }
    }
    httpRespond(client, 400);
  } else if (query.startsWith("/remove/")) {
    int stepi, stepn;
    int i = 8;
    if (parseIntUntil(query, stepi, i, '/')
        && parseIntUntil(query, stepn, i)
        && 0 <= stepi && stepi < program.stepCount
        && 0 < stepn && stepi + stepn <= program.stepCount) {
      for (int j = 0; j < stepn; j++) {
        program.steps[stepi] = program.steps[stepi + stepn];
        stepi++;
      }
      program.stepCount -= stepn;
      httpRespond(client, 200);
      return;
    }
    httpRespond(client, 400);
  } else if (query.equals("/run")) {
    httpRespond(client, 200);
    runMode = true;
  } else if (query.startsWith("/set/")) {
    Step step;
    stringToStep(query.substring(5), step);
    step.moveTo();
    httpRespond(client, 200);
#if WEBAPP
  } else {
    String path = request.substring(4, request.length() - 9);
    loadFromFlash(client, path);
#endif
  }
}
