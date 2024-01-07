//=======================================================================
// HLRobotController Firmware v0.0
//
// This sketch is the firmware on the HL RobotController.
// The device consists of a NodeMCU ESP8266 WiFi enabled microcontroller,
// 2 L298N dual channel DC motor controllers, one PCA9688? 16 channel PWM,
// and a couple of DC-DC converters.
//
// This provides a minimal web server for controlling the motors via
// web browser. When initially powered, it will try and connect to
// WiFi network using saved credentials. If that fails, it will create
// its own access point called "HLRobotController" that can be used 
// to enter the credentials of the local WiFi system which it will then
// save.
//
// Once connected to the local WiFi network, the web server can be used
// for controlling the motors via web browser.
//
// This firmware includes an OTA (Over-The_Air) updater (port 8266) that
// can be used to update the firmware over WiFi.
//
// This code is maintained at:
//
//  https://github.com/????
//=======================================================================


#include <Arduino.h>
#include <WiFiManager.h>
#include <NTPClient.h>
#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
#include <Wire.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ArduinoWebsockets.h>
#include <EEPROM.h>
#include <ArduinoOTA.h>
#include <Adafruit_GFX.h>
#include <string>
#include <sstream>
#include <vector>
#include <map>



// Name of Access Point (AP) to create when unable to connect to local WiFi
const char *APNAME = "HLMotorController";

// Setup our webserver which just serves up the static pages (with embedded javascript)
ESP8266WebServer server(80);

// Setup our websockets server for persistent connections
using namespace websockets;
WebsocketsServer webSocketServer;                 // main server for Websockets
std::vector<WebsocketsClient> webSocketClients;   // holds each Websockets connection

// For getting and keeping current time (see https://github.com/arduino-libraries/NTPClient)
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 0, 60*60*1000); // 0 is for adjusting timezone (0=UTC). Only update once/hr)

unsigned long html_page_requests = 0;

// 16chan PWM channel assignments
#define SERVO0   0 
#define SERVO1   1 
#define SERVO2   2 
#define SERVO3   3 
#define MOTOR0A  4
#define MOTOR0B  5
#define MOTOR1A  6
#define MOTOR1B  7
#define MOTOR2A  8
#define MOTOR2B  9
#define MOTOR3A 10
#define MOTOR3B 11
#define SERVO4  12
#define SERVO5  14 
#define SERVO6  14 
#define SERVO7  15

// Current motor settings (-1 to +1)
String MOTOR_NAMES[] = {"M0", "M1", "M2", "M3"};
bool MOTOR_ENABLE[4]; // C++ sets these all to false be default
float MOTOR_SET[5];

// Current servo settings
String SERVO_NAMES[] = {"S0", "S1", "S2", "S3", "S4", "S5", "S6", "S7"};
bool SERVO_ENABLE[8]; // C++ sets these all to false be default
float SERVO_SET[8];

//==========================================================================================================================

//-----------------------------------------
// getSingleChannelControlsHTML
//-----------------------------------------
String getSingleChannelControlsHTML(String name, float scale=1.0)
{
  String scale1(scale, 2);
  String scale10(scale*10.0, 2);
  String id = name + "_enable";
  String html = "";
  html += "    <input type=\"checkbox\" id=\"" + id + "\" />\n";
  html += "    <label for=\"" + id + "\">" + name + " enable</label>\n";
  html += "    <button onClick=\"sendCommand('incr " + name + " -" + scale10 +"')\">-" + scale10 + "</button>\n";
  html += "    <button onClick=\"sendCommand('incr " + name + " -" + scale   +"')\">-" + scale   + "</button>\n";
  html += "    <button onClick=\"sendCommand('incr " + name + "  " + scale   +"')\">+" + scale   + "</button>\n";
  html += "    <button onClick=\"sendCommand('incr " + name + "  " + scale10 +"')\">+" + scale10 + "</button>\n";
  html += "    <script>\n";
  html += "       document.getElementById('" + id + "').addEventListener('change', function() {\n";
  html += "         if (this.checked) { connection.send(\"enable " + name + " 1\"); }else{ connection.send(\"enable " + name + " 0\");}\n";
  html += "       });\n";
  html += "    </script>\n";

  return html;
}

//-----------------------------------------
// getHomePageHTML
//-----------------------------------------
String getHomePageHTML(void)
{
  String html = R"(
      <html>
        <body>
          <h1>HL Robot Controller</h1>
          <p>
          n.b. The controls here work with Firefox and Safari, but not Chrome.
          <p>
          <hr>
          <h2>Status</h2>
          <div id="status">Waiting for status...</div>
          <hr>

          <script>
            var connection = new WebSocket('ws://' + location.hostname + ':81/', ['arduino']);
            connection.onopen = function () { console.log('Connected: '); };
            connection.onerror = function (error) { console.log('WebSocket Error:: ', error); };
            connection.onmessage = function (e) {
              var message = JSON.parse(e.data);
              switch (message.type) {
                case 'status':
                    document.getElementById('status').innerHTML = message.content;
                    break;
                // Handle other message types here
                default:
                    console.log('Unknown message type');
              }
            };

            function sendCommand(command) {
              connection.send(command);
              }
          </script>
    )";

  // Add controls for each motor
  html += "\n";
  html += "<table>\n";
  html += "  <tr><th style='background-color: red; color: white; border: 1px solid black;'>DC Motors</th</tr>\n";
  for( auto name: MOTOR_NAMES ) html += "  <tr><td>\n" + getSingleChannelControlsHTML( name, 0.01 ) + "\n  </td></tr>\n";
  html += "</table>\n";
  html += "<table>\n";
  html += "  <tr><th style='background-color: black; color: white; border: 1px solid black;'>Servos</th</tr>\n";
  for( auto name: SERVO_NAMES ) html += "  <tr><td>\n" + getSingleChannelControlsHTML( name ) + "\n  </td></tr>\n";
  html += "</table>\n";

  // Close off HTML
  html += R"(
        </body>
      </html>
    )";
  
  return html;
}

//-----------------------------------------
// getStatusHTML
//-----------------------------------------
String getStatusHTML(void)
{
    String html = R"(
      <table style='border-collapse: collapse; border: 2px solid black; text-align: center;'>
        <tr style='background-color: green;'>
          <th style='color: white; border: 1px solid black;'>motor</th>
          <th style='color: white; border: 1px solid black;'>enabled</th>
          <th style='color: white; border: 1px solid black;'>setting</th>
        </tr>
    )";
    String types[] = {"M0", "M1", "M2", "M3", "S0", "S1", "S2", "S3", "S4", "S5", "S6", "S7"};

    for(int i = 0; i < 4; i++) {
        html += "<tr>";
        html += "<td style='border: 1px solid black;'>" + types[i] + "</td>";
        html += "<td style='border: 1px solid black;'>" + (String)(MOTOR_ENABLE[i] ? "Y" : "N") + "</td>";
        html += "<td style='border: 1px solid black;'>" + String(MOTOR_SET[i]) + "</td>";
        html += "</tr>";
    }

    for(int i = 0; i < 8; i++) {
        html += "<tr>";
        html += "<td style='border: 1px solid black;'>" + types[i+4] + "</td>";
        html += "<td style='border: 1px solid black;'>" + (String)(SERVO_ENABLE[i] ? "Y" : "N") + "</td>";
        html += "<td style='border: 1px solid black;'>" + String(SERVO_SET[i]) + "</td>";
        html += "</tr>";
    }

    html += "</table>";
    return html;

}

//-----------------------------------------
// escapeJsonString
//-----------------------------------------
String escapeJsonString(const String& input) {
    String output;
    for (char c : input) {
        switch (c) {
            case '\"': output += "\\\""; break; // Escape double quote
            case '\\': output += "\\\\"; break; // Escape backslash
            case '\n': output += "\\n"; break;  // Escape newlines
            // Add more cases here if you need to escape other characters
            default: output += c;
        }
    }
    return output;
}


//==========================================================================================================================

//-----------------------------------------
// SetMotor
//-----------------------------------------
void SetMotor(int idx, float val)
{
  if( idx<0 || idx>3 ){
    Serial.print("Bad Motor number: ");
    Serial.println(idx);
    return;
  }

  if( val< -1.0 ) val = -1.0;
  if( val> +1.0 ) val = +1.0;
  
  MOTOR_SET[idx] = val;

  
}

//==========================================================================================================================

//-----------------------------------------
// splitStringByWhitespace
//-----------------------------------------
std::vector<std::string> splitStringByWhitespace(const std::string &str) {
    std::istringstream iss(str);
    std::vector<std::string> tokens;
    std::string token;

    while (iss >> token) { tokens.push_back(token); }

    return tokens;
}

//-----------------------------------------
// onMessage
//
// This is the callback that handles incoming messages from WebSockets.
//-----------------------------------------
void onMessage(WebsocketsClient& client, WebsocketsMessage message) {
  String data = message.data();
  Serial.print("WS message received: ");
  Serial.println(data);

  auto tokens = splitStringByWhitespace(data.c_str());
  std::string cmd = tokens.empty() ? "none":tokens[0];
  if( cmd=="none"){
    Serial.println("Empty command received.");

  // enable/disable
  }else if( cmd=="enable" && tokens.size()==3){
    int idx = atoi(&(tokens[1].c_str()[1])); // TODO: check that tokens[1].length()==1
    bool enable = atoi(tokens[2].c_str());
    if( tokens[1][0] == 'M' ){
      if( idx>=0 && idx<=3 ){MOTOR_ENABLE[idx] = enable;}else{Serial.println("Bad Motor number in: " + data);}
    }
    if( tokens[1][0] == 'S' ){
      if( idx>=0 && idx<=7 ){SERVO_ENABLE[idx] = enable;}else{Serial.println("Bad Servo number in: " + data);}
    }

  // Motor/servo increment/decrement
  }else if( cmd=="incr" && tokens.size()==3){
    int idx = atoi(&(tokens[1].c_str()[1])); // TODO: check that tokens[1].length()==1
    float delta = atof(tokens[2].c_str());
    if( tokens[1][0] == 'M' ){
      if( idx>=0 && idx<=3 ){SetMotor(idx, MOTOR_SET[idx]+delta);}else{Serial.println("Bad Motor number in: " + data);}
    }
    if( tokens[1][0] == 'S' ){
      if( idx>=0 && idx<=7 ){SERVO_SET[idx] += delta;}else{Serial.println("Bad Servo number in: " + data);}
    }
  
  // Unknown or bad command
  }else{
    Serial.println("Bad command: " + data);
  }
}

//-----------------------------------------
// handleWebSockets
//
// This is called from loop() to handle new WebSockets connections,
// poll exisiting connections, and remove dead connections.
//-----------------------------------------
void handleWebSockets() {
  // Accept new WebSocket clients
  if (webSocketServer.poll()) {
    Serial.println("Accepting connection");
    WebsocketsClient client = webSocketServer.accept();
    if (client.available()) {
      Serial.println("Client available");
      client.onMessage([&client](WebsocketsMessage msg){ onMessage(client, msg);});
      webSocketClients.push_back(std::move(client));
    }
  }

  // Handle messages from all connected clients
  for (auto& client : webSocketClients) {
    client.poll(); // will automtically call onMessage() if message available
  }

  // Clean up disconnected clients
  webSocketClients.erase(
    std::remove_if(webSocketClients.begin(), webSocketClients.end(),
                   [](WebsocketsClient& c) {
                      if(!c.available()){
                        Serial.print("Removing WS client. Close reason: ");
                        Serial.println(c.getCloseReason());
                        return true;
                      }else{
                        return false;
                      }
                    }),
    webSocketClients.end());
  
  // Send status periodically to all connected clients
  static auto last_send = millis();
  if( millis()-last_send > 1000 ){

    Serial.println("Sending status ...");
    auto status_html = getStatusHTML();
    String statusMessage = "{\"type\": \"status\", \"content\": \"" + escapeJsonString(status_html) + "\"}";
    for(auto &client : webSocketClients){
      client.send(statusMessage);
    }
    last_send = millis();
  }
}



//==========================================================================================================================

//-----------------------------------------
// setup
//-----------------------------------------
void setup() {
// Start serial monitor
  Serial.begin(74880); // the ESP8266 boot loader uses this. We can set it to something else here, but the first few messages will be garbled.
  Serial.println();

  // Connect to WiFi
  // This will use cached credentials to try and connect. If unsuccessful,
  // it will automatically setup an AP where the credentials can be set
  // via web browser.
  WiFiManager wm;
  bool res = wm.autoConnect(APNAME); 

  // If we failed to connect to WiFi, restart the device so we can try again.
  if(!res) {
    Serial.println("Failed to connect");
    ESP.restart();
  }

  // If we get here, we're connected to WiFi
  Serial.println(" connected");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());  // Print the local IP address

  // Connect to ntp Time Server
  Serial.printf("Connecting to ntp server to get current time ...\n");
  timeClient.begin();
  timeClient.update(); // n.b. this does not block and is not required to succeed

  // Start websocket server which will handle persistent connections from javascript
  webSocketServer.listen(81);
  Serial.print("Websockets Server Status: ");
  Serial.println(webSocketServer.available());

  //...............................................................................
  // Setup to allow Over The Air (OTA) updates to the firmware. This allows the
  // program to be updated via WiFi without having to connect via USB.
  // NOTE: To use this, select the approriate IP address for the serial port in
  // the Arduino IDE. This will allow uploading the new firmware but
  // IT WILL NOT ALLOW SERIAL MONITORING OVER THE WiFi!!!

  // Set password for firmware updates
  ArduinoOTA.setPassword("314159");

  ArduinoOTA.onStart([]() {
    String type;
    if (ArduinoOTA.getCommand() == U_FLASH) {
      type = "sketch";
    } else {  // U_FS
      type = "filesystem";
    }

    // NOTE: if updating FS this would be the place to unmount FS using FS.end()
    Serial.println("Start updating " + type);
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\nEnd");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) {
      Serial.println("Auth Failed");
    } else if (error == OTA_BEGIN_ERROR) {
      Serial.println("Begin Failed");
    } else if (error == OTA_CONNECT_ERROR) {
      Serial.println("Connect Failed");
    } else if (error == OTA_RECEIVE_ERROR) {
      Serial.println("Receive Failed");
    } else if (error == OTA_END_ERROR) {
      Serial.println("End Failed");
    }
  });
  ArduinoOTA.begin();
  //...............................................................................
  
  // Start HTTP Server for Web Page
  server.on("/", HTTP_GET, []() { server.send(200, "text/html", getHomePageHTML());});
  server.begin();
  
}

//-----------------------------------------
// loop
//-----------------------------------------
void loop() {
  ArduinoOTA.handle();   // allows firmware upgrade over WiFi
  timeClient.update();   // keeps system clock updated

  server.handleClient(); // Handle HTTP requests
  handleWebSockets();    // Handle WebSocket connections
}
