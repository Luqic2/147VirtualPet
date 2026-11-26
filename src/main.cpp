#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <Preferences.h>
#include <Wire.h>
#include <Adafruit_BMP280.h>
#include <SparkFun_ATECCX08a_Arduino_Library.h>
#include <mbedtls/md.h>

#include "iot_common.h"
#include "iot_display.h"
#include "iot_provision.h"
#include "iot_ca_certs.h"
#include "interact.h"

SerialLogger SerialLog;

// Topics (replace T00 with your team ID):
//   iot/T00/telemetry      JSON, every PERIOD_MS (the exact bytes that get signed)
//   iot/T00/telemetry/sig  {"seq":42,"alg":"ES256","sig":"<128 hex chars = r||s>"}
//   iot/T00/pubkey         <128 hex chars = X||Y>, retained: the P-256 public key of slot 0
//   iot/T00/status         "online" / "offline", retained, "offline" is the Last Will
//
// Serial commands at startup:  PROVISION (re-enter credentials), FORGET (erase them)
// Serial commands while running: HELP, STATUS, PUB, PUBKEY, CA GOOD, CA WRONG, PROVISION, FORGET

// ---------------- Team settings (not secret) ----------------
#define TEAM_ID "T02"   // your team number from Canvas, e.g. "T07"

constexpr uint16_t MQTT_PORT        = 8883;    // MQTT over TLS
constexpr uint16_t MQTT_KEEPALIVE_S = 15;
constexpr uint32_t WIFI_RETRY_MS    = 20000;
constexpr uint32_t BACKOFF_START_MS = 1000;
constexpr uint32_t BACKOFF_MAX_MS   = 30000;
constexpr uint32_t PERIOD_MS        = 10000;

// ---------------- Globals ----------------
struct Credentials {
  String wifiSsid, wifiPass;
  String mqttHost;
  String mqttUser, mqttPass;
};
Credentials creds;

Preferences prefs;
// Same namespace as Lab 5, so a board provisioned in Lab 5 keeps its credentials.
constexpr const char* NVS_NAMESPACE = "iot-lab5";

WiFiClientSecure net;          // TLS socket
PubSubClient mqtt(net);        // MQTT on top of the TLS socket

const String TOPIC_TELEMETRY = String("iot/") + TEAM_ID + "/telemetry";
const String TOPIC_SIG       = String("iot/") + TEAM_ID + "/telemetry/sig";
const String TOPIC_PUBKEY    = String("iot/") + TEAM_ID + "/pubkey";
const String TOPIC_STATUS    = String("iot/") + TEAM_ID + "/status";
String clientId;

Adafruit_BMP280 bmp(&Wire);
bool bmpOk = false;

ATECCX08A atecc;
bool eccSigning = false;       // true only if the chip is locked (provisioned) and we read the key
uint8_t devicePubKey[64];      // X||Y of the slot 0 key
uint32_t sigOk = 0, sigFail = 0;

bool useWrongCa = false;       // "CA WRONG" demo: verification must make the handshake fail

uint32_t backoffMs   = BACKOFF_START_MS;
uint32_t lastMqttTry = 0;
uint32_t seqNo       = 0;
uint32_t pubOk = 0, pubFail = 0, reconnects = 0;

// Bytes -> lowercase hex. out needs 2*len+1 chars.
void toHex(const uint8_t* data, size_t len, char* out) {
  static const char* digits = "0123456789abcdef";
  for (size_t i = 0; i < len; i++) {
    out[2 * i]     = digits[data[i] >> 4];
    out[2 * i + 1] = digits[data[i] & 0x0F];
  }
  out[2 * len] = '\0';
}

// ============================================================================================
// Credentials in NVS
// ============================================================================================

bool loadCredentials() {
  prefs.begin(NVS_NAMESPACE, true);    // read-only
  creds.wifiSsid = prefs.getString("wifi_ssid", "");
  creds.wifiPass = prefs.getString("wifi_pass", "");
  creds.mqttHost = prefs.getString("mqtt_host", "");
  creds.mqttUser = prefs.getString("mqtt_user", "");
  creds.mqttPass = prefs.getString("mqtt_pass", "");
  prefs.end();
  return creds.wifiSsid.length() > 0 && creds.mqttHost.length() > 0 &&
         creds.mqttUser.length() > 0;
}

void saveCredentials() {
  prefs.begin(NVS_NAMESPACE, false);
  prefs.putString("wifi_ssid", creds.wifiSsid);
  prefs.putString("wifi_pass", creds.wifiPass);
  prefs.putString("mqtt_host", creds.mqttHost);
  prefs.putString("mqtt_user", creds.mqttUser);
  prefs.putString("mqtt_pass", creds.mqttPass);
  prefs.end();
}

void forgetCredentials() {
  prefs.begin(NVS_NAMESPACE, false);
  prefs.clear();
  prefs.end();
  creds = Credentials();
  SerialLog.println("Credentials erased from NVS.");
}

void printCredentials() {
  SerialLog.print("  Wi-Fi SSID    : "); SerialLog.println(creds.wifiSsid);
  SerialLog.print("  Wi-Fi password: "); SerialLog.println(iotMasked(creds.wifiPass));
  SerialLog.print("  MQTT host     : "); SerialLog.println(creds.mqttHost);
  SerialLog.print("  MQTT username : "); SerialLog.println(creds.mqttUser);
  SerialLog.print("  MQTT password : "); SerialLog.println(iotMasked(creds.mqttPass));
}

void runProvisioning() {
  SerialLog.println();
  SerialLog.println("=== Provisioning ===");
  SerialLog.println("Values are stored in this board's NVS flash, never in your source code.");
  do {
    creds.wifiSsid = iotAsk("Wi-Fi SSID:");
  } while (creds.wifiSsid.length() == 0);
  creds.wifiPass = iotAsk("Wi-Fi password (leave empty for an open network):");
  do {
    creds.mqttHost = iotCleanHost(iotAsk("MQTT host (HiveMQ Cluster URL, e.g. abc123.s1.eu.hivemq.cloud):"));
  } while (creds.mqttHost.length() == 0);
  do {
    creds.mqttUser = iotAsk("MQTT username:");
  } while (creds.mqttUser.length() == 0);
  creds.mqttPass = iotAsk("MQTT password:");
  saveCredentials();
  SerialLog.println("Saved to NVS:");
  printCredentials();
}

// ============================================================================================
// MQTT over TLS, broker certificate verified
// ============================================================================================

void configureMqtt() {
  // Verify the broker. Give the TLS socket the root CA that signed the broker's
  // chain.
  // The ESP32 then checks the chain up to this root AND that the certificate name
  // matches creds.mqttHost. If either check fails, the handshake fails.
  // Use WRONG_CA_PEM instead when useWrongCa is true (console command CA WRONG),
  // and print the CA that is being used.
  if (useWrongCa) {
    SerialLog.println("TLS: verifying the broker with the WRONG CA (ISRG Root X1): expect a failure");
    net.setCACert(WRONG_CA_PEM);
  } else {
    SerialLog.println("TLS: verifying the broker with DigiCert Global Root CA (Let's Encrypt)");
    net.setCACert(DIGICERT_GLOBAL_ROOT_CA_PEM);
  }

  mqtt.setServer(creds.mqttHost.c_str(), MQTT_PORT);   // pointer kept: creds must stay put
  mqtt.setKeepAlive(MQTT_KEEPALIVE_S);
  mqtt.setSocketTimeout(5);
  mqtt.setBufferSize(512);   // telemetry + a 128-char signature
}

const char* mqttStateText(int s) {
  switch (s) {
    case MQTT_CONNECTION_TIMEOUT:      return "-4 CONNECTION_TIMEOUT (no answer: host/port/firewall?)";
    case MQTT_CONNECTION_LOST:         return "-3 CONNECTION_LOST (network dropped)";
    case MQTT_CONNECT_FAILED:          return "-2 CONNECT_FAILED (TCP/TLS failed: see the TLS error line)";
    case MQTT_DISCONNECTED:            return "-1 DISCONNECTED";
    case MQTT_CONNECTED:               return "0 CONNECTED";
    case MQTT_CONNECT_BAD_PROTOCOL:    return "1 BAD_PROTOCOL";
    case MQTT_CONNECT_BAD_CLIENT_ID:   return "2 BAD_CLIENT_ID";
    case MQTT_CONNECT_UNAVAILABLE:     return "3 UNAVAILABLE (broker busy)";
    case MQTT_CONNECT_BAD_CREDENTIALS: return "4 BAD_CREDENTIALS (username/password)";
    case MQTT_CONNECT_UNAUTHORIZED:    return "5 UNAUTHORIZED (credential lacks permission)";
    default:                           return "unknown";
  }
}

bool mqttConnect() {
  SerialLog.print("MQTT: connecting to ");
  SerialLog.print(creds.mqttHost);
  SerialLog.print(":");
  SerialLog.print(MQTT_PORT);
  SerialLog.print(" as ");
  SerialLog.println(clientId);
  bool ok = mqtt.connect(clientId.c_str(),
                         creds.mqttUser.c_str(), creds.mqttPass.c_str(),
                         TOPIC_STATUS.c_str(), 1, true, "offline");
  if (!ok) {
    // Why did TLS fail? With the wrong CA this prints an X509 verification error.
    char err[128];
    int code = net.lastError(err, sizeof err);
    if (code != 0) {
      SerialLog.print("TLS error ");
      SerialLog.print(code);
      SerialLog.print(": ");
      SerialLog.println(err);
    }
    return false;
  }
  mqtt.publish(TOPIC_STATUS.c_str(), "online", true);
  if (eccSigning) {
    char hex[129];
    toHex(devicePubKey, 64, hex);
    mqtt.publish(TOPIC_PUBKEY.c_str(), hex, true);   // retained, so a verifier can find it
  }
  return true;
}

void maintainWiFi() {
  static uint32_t lastBegin = 0;
  static bool started = false, wasUp = false;
  const bool up = (WiFi.status() == WL_CONNECTED);
  if (up && !wasUp) { SerialLog.print("Wi-Fi: connected, IP "); SerialLog.println(WiFi.localIP()); }
  if (!up && wasUp) SerialLog.println("Wi-Fi: connection lost");
  wasUp = up;
  if (!up && (!started || millis() - lastBegin >= WIFI_RETRY_MS)) {
    started = true;
    lastBegin = millis();
    SerialLog.print("Wi-Fi: connecting to ");
    SerialLog.println(creds.wifiSsid);
    WiFi.begin(creds.wifiSsid.c_str(), creds.wifiPass.c_str());
  }
}

void maintainMqtt() {
  if (WiFi.status() != WL_CONNECTED || mqtt.connected()) return;
  const uint32_t now = millis();
  if (now - lastMqttTry < backoffMs) return;
  lastMqttTry = now;
  if (mqttConnect()) {
    SerialLog.println("MQTT: connected over TLS, broker certificate verified");
    backoffMs = BACKOFF_START_MS;
    reconnects++;
  } else {
    SerialLog.print("MQTT: failed, state ");
    SerialLog.println(mqttStateText(mqtt.state()));
    backoffMs = std::min<uint32_t>(backoffMs * 2, BACKOFF_MAX_MS) + (millis() % 500);
    SerialLog.print("MQTT: next try in ");
    SerialLog.print(backoffMs / 1000.0f, 1);
    SerialLog.println(" s");
  }
}

// ============================================================================================
// Device key on the ATECC608A (read-only use, never lock anything)
// ============================================================================================

void initSecureElement() {
  if (!atecc.begin(IoTPins::ATECC608A_ADDR, Wire, Serial)) {
    SerialLog.println("ATECC608A not found at 0x60 (check the Qwiic cable). Continuing without signing.");
    return;
  }

  // Read the configuration zone, then check atecc.configLockStatus and
  // atecc.dataOTPLockStatus.
  if (!atecc.readConfigZone(false)) {
    SerialLog.println("ATECC608A configuration zone read failed. Continuing without signing.");
    return;
  }

  char serialHex[SERIAL_NUMBER_SIZE * 2 + 1];
  toHex(atecc.serialNumber, SERIAL_NUMBER_SIZE, serialHex);
  SerialLog.print("ATECC608A serial: ");
  SerialLog.println(serialHex);
  SerialLog.print("  config locked: ");
  SerialLog.println(atecc.configLockStatus ? "yes" : "no");
  SerialLog.print("  data/OTP locked: ");
  SerialLog.println(atecc.dataOTPLockStatus ? "yes" : "no");
  SerialLog.print("  slot 0 locked: ");
  SerialLog.println(atecc.slot0LockStatus ? "yes" : "no");

  if (!atecc.configLockStatus || !atecc.dataOTPLockStatus || !atecc.slot0LockStatus) {
    SerialLog.println("ATECC608A NOT PROVISIONED — ask a TA (do not lock it yourself)");
    return;
  }

  // Read the public key that matches the private key in slot 0.
  // Copy it to devicePubKey, print it as hex, and set eccSigning = true.
  // (generatePublicKey computes the public half; the private key never leaves the chip.)
  if (!atecc.generatePublicKey(0, false)) {
    SerialLog.println("ATECC608A public key read failed. Continuing without signing.");
    return;
  }
  memcpy(devicePubKey, atecc.publicKey64Bytes, sizeof devicePubKey);
  char publicKeyHex[sizeof devicePubKey * 2 + 1];
  toHex(devicePubKey, sizeof devicePubKey, publicKeyHex);
  SerialLog.print("ATECC608A public key: ");
  SerialLog.println(publicKeyHex);
  eccSigning = true;

}

// Sign the exact payload bytes. sigHex needs 129 chars. Returns false on any error.
bool signPayload(const char* payload, char* sigHex) {
  // Hash the payload: SHA-256 over strlen(payload) bytes into digest[32].
  // Returns 0 on success.
  uint8_t digest[32] = {0};
  if (payload == nullptr || sigHex == nullptr ||
      mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                 reinterpret_cast<const uint8_t*>(payload), strlen(payload), digest) != 0) {
    return false;
  }

  // Let the chip sign the digest with the slot 0 key: atecc.createSignature(digest, 0).
  // The 64-byte signature (r||s) is in atecc.signature; convert it to hex in sigHex.
  if (!atecc.createSignature(digest, 0)) return false;
  toHex(atecc.signature, 64, sigHex);
  return true;
}

// ============================================================================================
// Telemetry
// ============================================================================================

int readPotPercent() {
  return constrain((int)map(analogRead(IoTPins::POT_ADC), 0, 4095, 0, 100), 0, 100);
}

bool publishTelemetry() {
  if (!mqtt.connected()) return false;

  char temp[16] = "null", press[16] = "null";
  if (bmpOk) {
    snprintf(temp,  sizeof temp,  "%.2f", bmp.readTemperature());
    snprintf(press, sizeof press, "%.2f", bmp.readPressure() / 100.0f);
  }
  char buf[192];
  snprintf(buf, sizeof buf,
           "{\"seq\":%lu,\"temp_f\":%s,\"press_hpa\":%s,\"pot_pct\":%d,\"rssi\":%d,\"uptime_s\":%lu}",
           (unsigned long)(++seqNo), temp, press, readPotPercent(), (int)WiFi.RSSI(),
           (unsigned long)(millis() / 1000));
  bool ok = mqtt.publish(TOPIC_TELEMETRY.c_str(), buf, false);
  SerialLog.print(ok ? "PUB " : "PUB FAILED ");
  SerialLog.print(TOPIC_TELEMETRY);
  SerialLog.print("  ");
  SerialLog.println(buf);

  if (ok && eccSigning) {
    char sigHex[129];
    if (signPayload(buf, sigHex)) {
      char sigMsg[200];
      snprintf(sigMsg, sizeof sigMsg, "{\"seq\":%lu,\"alg\":\"ES256\",\"sig\":\"%s\"}",
               (unsigned long)seqNo, sigHex);
      mqtt.publish(TOPIC_SIG.c_str(), sigMsg, false);
      SerialLog.print("SIG ");
      SerialLog.println(sigHex);
      sigOk++;
    } else {
      SerialLog.println("SIG FAILED (ATECC608A did not sign)");
      sigFail++;
    }
  }
  return ok;
}

// ============================================================================================
// Serial console (given)
// ============================================================================================

void printHelp() {
  SerialLog.println("Commands: HELP | STATUS | PUB | PUBKEY | CA GOOD | CA WRONG | PROVISION | FORGET");
}

void printStatus() {
  SerialLog.print("Team "); SerialLog.print(TEAM_ID); SerialLog.print("  client id "); SerialLog.println(clientId);
  printCredentials();
  SerialLog.print("  Wi-Fi: "); SerialLog.println(WiFi.status() == WL_CONNECTED ? "connected" : "down");
  SerialLog.print("  MQTT : "); SerialLog.println(mqttStateText(mqtt.state()));
  SerialLog.print("  TLS CA: "); SerialLog.println(useWrongCa ? "WRONG CA (demo)" : "ISRG Root X1");
  SerialLog.print("  signing: "); SerialLog.println(eccSigning ? "ATECC608A slot 0" : "off");
  SerialLog.print("  published ok/failed: "); SerialLog.print(pubOk); SerialLog.print("/"); SerialLog.print(pubFail);
  SerialLog.print("  signed ok/failed: "); SerialLog.print(sigOk); SerialLog.print("/"); SerialLog.println(sigFail);
}

void goOfflineCleanly() {
  if (mqtt.connected()) {
    mqtt.publish(TOPIC_STATUS.c_str(), "offline", true);
    mqtt.disconnect();
  }
}

void handleCommand(String line) {
  line.trim();
  line.toUpperCase();
  if (line.length() == 0) return;
  if (line == "HELP")   { printHelp(); return; }
  if (line == "STATUS") { printStatus(); return; }
  if (line == "PUB") {
    if (!mqtt.connected()) { SerialLog.println("ERR not connected to the broker"); return; }
    if (publishTelemetry()) pubOk++; else pubFail++;
    return;
  }
  if (line == "PUBKEY") {
    if (!eccSigning) { SerialLog.println("ERR no device key (chip missing or not provisioned)"); return; }
    char hex[129];
    toHex(devicePubKey, 64, hex);
    SerialLog.println(hex);
    return;
  }
  if (line == "CA WRONG" || line == "CA GOOD") {
    useWrongCa = (line == "CA WRONG");
    goOfflineCleanly();
    net.stop();
    configureMqtt();
    backoffMs = BACKOFF_START_MS;
    lastMqttTry = millis() - backoffMs;   // try again right away
    SerialLog.println("OK reconnecting with the selected CA");
    return;
  }
  if (line == "FORGET") { goOfflineCleanly(); forgetCredentials(); SerialLog.println("Rebooting..."); ESP.restart(); return; }
  if (line == "PROVISION") { goOfflineCleanly(); runProvisioning(); SerialLog.println("Rebooting..."); ESP.restart(); return; }
  SerialLog.println("ERR unknown command. Type HELP");
}

void pollConsole() {
  static String buf;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') { handleCommand(buf); buf = ""; continue; }
    if (buf.length() < 60) buf += c;
  }
}

// ============================================================================================

void setup() {
  iotStartSerial("Virtual Pet");
  iotInitI2C();   // default 100 kHz: the ATECC608A wake-up pulse relies on it
  iotInitSPI();
  iotInitDisplay();
  pinMode(IoTPins::POT_ADC, INPUT);

  bmpOk = bmp.begin(IoTPins::BMP280_ADDR);
  SerialLog.println(bmpOk ? "BMP280 OK at 0x76" : "BMP280 not found (temp/pressure will be null)");
  petBegin();

  initSecureElement();

  uint64_t chip = ESP.getEfuseMac();
  char idbuf[32];
  snprintf(idbuf, sizeof idbuf, "iot-%s-%08lX", TEAM_ID, (unsigned long)(chip & 0xFFFFFFFF));
  clientId = idbuf;

  SerialLog.println("Type PROVISION (new credentials) or FORGET (erase) within 3 s...");
  iotDisplayLines("Lab 6 harden", "PROVISION/FORGET?", "(3 s, Serial)");
  String cmd = iotReadLine(3000);
  cmd.toUpperCase();
  if (cmd == "FORGET") forgetCredentials();

  bool haveCreds = loadCredentials();
  if (cmd == "PROVISION" || !haveCreds) {
    if (!haveCreds) SerialLog.println("No credentials in NVS yet.");
    iotDisplayLines("Lab 6 harden", "Provisioning:", "use Serial Monitor");
    runProvisioning();
  } else {
    SerialLog.println("Credentials loaded from NVS:");
    printCredentials();
  }

  WiFi.mode(WIFI_STA);
  configureMqtt();
  printHelp();
}

void loop() {
  pollConsole();
  maintainWiFi();
  maintainMqtt();
  mqtt.loop();

  static uint32_t lastPub = 0;
  if (iotEvery(PERIOD_MS, lastPub)) {
    if (mqtt.connected()) {
      if (publishTelemetry()) pubOk++; else pubFail++;
    }
  }

  static uint32_t lastPetSample = millis() - 5000;
  static float petTemperatureF = 0.0f;
  static bool petTemperatureAvailable = false;
  if (iotEvery(5000, lastPetSample)) {
    petTemperatureAvailable = bmpOk;

    if (petTemperatureAvailable) {
        float temperatureC = bmp.readTemperature();
        petTemperatureF = (temperatureC * 9.0f / 5.0f) + 32.0f;
    }
  }
  petUpdate(petTemperatureF, petTemperatureAvailable, readPotPercent());
}
