// Sony Alpha BLE tally light -- Adafruit QT Py ESP32-S3 + onboard NeoPixel
// Arduino / NimBLE-Arduino 2.x
//
// Bonds to the camera as a Bluetooth remote commander, subscribes to the status
// characteristic (0xFF02) and drives the NeoPixel from the "recording"
// notification (0x02 0xD5 0x20 = started, 0x02 0xD5 0x00 = stopped).
//
// NeoPixel states:
//   dim blue      scanning for a camera
//   dim yellow    connecting / bonding
//   dim magenta   connected but no remote service (Bluetooth Rmt Ctrl Off?)
//   dim white     connected, recording state not yet reported
//   dim green     connected, not recording
//   bright red    RECORDING
//
// Camera setup (a7 IV): MENU > Network > Bluetooth > Bluetooth Function: On,
// Bluetooth Rmt Ctrl: On. For the FIRST bond you must also be sitting on
// MENU > Network > Bluetooth > Pairing -- the camera only accepts new bonds
// while that screen is open (advertised mode22 bit 0x40 goes high).
//
// Hold BOOT while resetting to erase all stored bonds on this board.
// Tap BOOT while connected to send a record toggle.
//
// Set Tools > Core Debug Level to Verbose to see NimBLE's own SM logs.

#include <Adafruit_NeoPixel.h>
#include <Arduino.h>
#include <NimBLEDevice.h>

#define BOOT_BTN 0

// ---------------------------------------------------------------- NeoPixel
// PIN_NEOPIXEL = 39, NEOPIXEL_POWER = 38 on the QT Py ESP32-S3. The power pin
// MUST be driven HIGH or the pixel stays dark.
static Adafruit_NeoPixel pixel(NEOPIXEL_NUM, PIN_NEOPIXEL, NEO_GRB + NEO_KHZ800);

static const uint8_t IDLE_BRIGHT = 10;
static const uint8_t REC_BRIGHT = 160;

enum class UiState : uint8_t { Scanning, Connecting, NoService, Connected, Idle, Recording };

static volatile UiState g_ui = UiState::Scanning;
static UiState g_uiShown = UiState::Recording;  // force a first paint

static void paint(UiState s) {
  uint8_t r = 0, g = 0, b = 0;
  switch (s) {
    case UiState::Scanning:   b = IDLE_BRIGHT; break;
    case UiState::Connecting: r = IDLE_BRIGHT; g = IDLE_BRIGHT; break;
    case UiState::NoService:  r = IDLE_BRIGHT; b = IDLE_BRIGHT; break;
    case UiState::Connected:  r = IDLE_BRIGHT; g = IDLE_BRIGHT; b = IDLE_BRIGHT; break;
    case UiState::Idle:       g = IDLE_BRIGHT; break;
    case UiState::Recording:  r = REC_BRIGHT; break;
  }
  pixel.setPixelColor(0, pixel.Color(r, g, b));
  pixel.show();
}

// ---------------------------------------------------------------- Protocol
static const NimBLEUUID SVC_REMOTE{0x8000ff00, 0xff00, 0xffff, 0xffffffffffffffffULL};
static const NimBLEUUID CHR_COMMAND{(uint16_t)0xff01};
static const NimBLEUUID CHR_STATUS{(uint16_t)0xff02};

static const uint8_t STATUS_FOCUS = 0x3f;
static const uint8_t STATUS_SHUTTER = 0xa0;
static const uint8_t STATUS_RECORDING = 0xd5;

static const uint16_t CMD_RECORD_DOWN = 0x0e01;
static const uint16_t CMD_RECORD_UP = 0x0f01;

struct __attribute__((packed)) sony_adv_t {
  uint16_t company_id;  // 0x012d
  uint16_t type;        // 0x0003 = camera
  uint8_t protocol_version;
  uint8_t unused;
  uint16_t model;
  uint8_t tag22;  // 0x22
  uint8_t mode22;
  uint8_t zero0;
  uint8_t tag21;
  uint8_t mode21;
};

static const uint16_t ADV_SONY_ID = 0x012d;
static const uint16_t ADV_SONY_CAMERA = 0x0003;
static const uint8_t ADV_PAIRING_SUPPORTED = 1 << 7;  // 0x80
static const uint8_t ADV_PAIRING_ENABLED = 1 << 6;    // 0x40  <- Pairing screen open
static const uint8_t ADV_LOCATION_SUPPORTED = 1 << 5; // 0x20
static const uint8_t ADV_LOCATION_ENABLED = 1 << 4;   // 0x10
static const uint8_t ADV_REMOTE_ENABLED = 1 << 1;     // 0x02

static NimBLEAddress g_camera;
static bool g_haveCamera = false;
static bool g_pairingOpen = false;
static NimBLERemoteCharacteristic *g_command = nullptr;
static uint8_t g_attempts = 0;

// ---------------------------------------------------------------- Diagnostics
static const char *smErrName(uint8_t e) {
  switch (e) {
    case 0x01: return "PASSKEY";
    case 0x02: return "OOB";
    case 0x03: return "AUTHREQ (security requirements mismatch)";
    case 0x04: return "CONFIRM_MISMATCH";
    case 0x05: return "PAIR_NOT_SUPP (peer refuses to pair)";
    case 0x06: return "ENC_KEY_SZ";
    case 0x07: return "CMD_NOT_SUPP";
    case 0x08: return "UNSPECIFIED";
    case 0x09: return "REPEATED (peer thinks we already bonded -- stale key)";
    case 0x0a: return "INVAL";
    case 0x0b: return "DHKEY";
    case 0x0c: return "NUMCMP (numeric comparison failed/declined)";
    case 0x0d: return "ALREADY";
    case 0x0e: return "CROSS_TRANS";
    case 0x0f: return "KEY_REJ";
    default: return "?";
  }
}

static const char *hsErrName(int rc) {
  switch (rc) {
    case 2:  return "EALREADY";
    case 3:  return "EINVAL";
    case 5:  return "ENOENT";
    case 6:  return "ENOMEM";
    case 7:  return "ENOTCONN (link gone before pairing finished -- camera hung up)";
    case 8:  return "ENOTSUP";
    case 13: return "ETIMEOUT";
    case 16: return "EREJECT";
    case 23: return "EAUTHEN";
    case 25: return "EENCRYPT";
    case 27: return "ESTORE_CAP (bond slots full -- erase bonds)";
    case 28: return "ESTORE_FAIL";
    default: return nullptr;
  }
}

static const char *hciErrName(uint8_t e) {
  switch (e) {
    case 0x05: return "AUTH_FAIL";
    case 0x06: return "PINKEY_MISSING (one side lost the bond keys)";
    case 0x08: return "CONN_SPVN_TMO (supervision timeout)";
    case 0x13: return "REM_USER_CONN_TERM (the CAMERA hung up on us)";
    case 0x16: return "CONN_TERM_LOCAL (we hung up)";
    case 0x1f: return "UNSPECIFIED";
    case 0x22: return "LL_RSP_TMO";
    case 0x3e: return "CONN_ESTABLISHMENT failed";
    default: return "?";
  }
}

static void explainError(int rc) {
  if (rc == 0) {
    Serial.println("    rc = 0 (no error recorded)");
    return;
  }
  Serial.printf("    rc = 0x%04x", rc);
  if (rc >= 0x500 && rc < 0x600) {
    Serial.printf("  -> PEER SM error 0x%02x: %s", rc - 0x500, smErrName(rc - 0x500));
  } else if (rc >= 0x400 && rc < 0x500) {
    Serial.printf("  -> our SM error 0x%02x: %s", rc - 0x400, smErrName(rc - 0x400));
  } else if (rc >= 0x200 && rc < 0x300) {
    Serial.printf("  -> HCI 0x%02x: %s", rc - 0x200, hciErrName(rc - 0x200));
  } else if (rc >= 0x100 && rc < 0x200) {
    Serial.printf("  -> ATT error 0x%02x", rc - 0x100);
  } else {
    const char *n = hsErrName(rc);
    Serial.printf("  -> BLE_HS_%s", n ? n : "?");
  }
  Serial.println();
}

static void printAdvFlags(uint8_t mode22) {
  Serial.printf("  mode22 = 0x%02x  ->", mode22);
  Serial.printf(" pairing_supported=%d", (mode22 & ADV_PAIRING_SUPPORTED) ? 1 : 0);
  Serial.printf(" PAIRING_OPEN=%d", (mode22 & ADV_PAIRING_ENABLED) ? 1 : 0);
  Serial.printf(" location_supported=%d", (mode22 & ADV_LOCATION_SUPPORTED) ? 1 : 0);
  Serial.printf(" location_enabled=%d", (mode22 & ADV_LOCATION_ENABLED) ? 1 : 0);
  Serial.printf(" remote_enabled=%d\n", (mode22 & ADV_REMOTE_ENABLED) ? 1 : 0);
}

static void printBonds() {
  const int n = NimBLEDevice::getNumBonds();
  Serial.printf("stored bonds: %d\n", n);
  for (int i = 0; i < n; i++) {
    Serial.printf("  [%d] %s\n", i, NimBLEDevice::getBondedAddress(i).toString().c_str());
  }
}

// ---------------------------------------------------------------- Callbacks
class ClientCB : public NimBLEClientCallbacks {
  uint32_t m_connectedAt = 0;
  void onConnect(NimBLEClient *c) override {
    m_connectedAt = millis();
    Serial.printf("  onConnect: %s\n", c->getPeerAddress().toString().c_str());
  }
  void onDisconnect(NimBLEClient *c, int reason) override {
    Serial.printf("  onDisconnect after %lu ms: reason 0x%04x\n",
                  (unsigned long)(millis() - m_connectedAt), reason);
    explainError(reason);
  }
  void onConnectFail(NimBLEClient *c, int reason) override {
    Serial.printf("  onConnectFail: reason 0x%04x\n", reason);
    explainError(reason);
  }
  void onConfirmPasskey(NimBLEConnInfo &info, uint32_t pin) override {
    Serial.printf("  onConfirmPasskey: numeric comparison %06u -> accepting\n", pin);
    NimBLEDevice::injectConfirmPasskey(info, true);
  }
  void onPassKeyEntry(NimBLEConnInfo &info) override {
    Serial.println("  onPassKeyEntry: camera wants a passkey (unexpected) -> 0");
    NimBLEDevice::injectPassKey(info, 0);
  }
  void onAuthenticationComplete(NimBLEConnInfo &info) override {
    Serial.printf("  onAuthenticationComplete: encrypted=%d authenticated=%d bonded=%d keysz=%d\n",
                  info.isEncrypted(), info.isAuthenticated(), info.isBonded(),
                  info.getSecKeySize());
  }
  void onIdentity(NimBLEConnInfo &info) override {
    Serial.printf("  onIdentity: id addr %s\n", info.getIdAddress().toString().c_str());
  }
};

static ClientCB g_clientCB;

static void onStatus(NimBLERemoteCharacteristic *chr, uint8_t *data, size_t len, bool isNotify) {
  // NimBLE host task -- only set state here, paint from loop().
  Serial.print("0xff02 <-");
  for (size_t i = 0; i < len; i++) {
    Serial.printf(" %02x", data[i]);
  }
  Serial.println();

  if (len < 3 || data[0] != 0x02) {
    return;
  }
  const bool on = (data[2] & 0x20) != 0;
  switch (data[1]) {
    case STATUS_RECORDING:
      g_ui = on ? UiState::Recording : UiState::Idle;
      Serial.printf("  >> RECORDING = %d\n", on);
      break;
    case STATUS_FOCUS:
      Serial.printf("  focus = %d\n", on);
      break;
    case STATUS_SHUTTER:
      Serial.printf("  shutter = %d\n", on);
      break;
    default:
      break;
  }
}

class ScanCB : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice *dev) override {
    if (!dev->haveManufacturerData() || dev->getManufacturerData().size() < sizeof(sony_adv_t)) {
      return;
    }
    const auto adv = dev->getManufacturerData<sony_adv_t>();
    if (adv.company_id != ADV_SONY_ID || adv.type != ADV_SONY_CAMERA) {
      return;
    }
    Serial.printf("camera: %s (%s) model=0x%04x\n", dev->getName().c_str(),
                  dev->getAddress().toString().c_str(), adv.model);
    printAdvFlags(adv.mode22);

    if (!(adv.mode22 & ADV_REMOTE_ENABLED)) {
      Serial.println("  !! remote control is OFF on the camera -- skipping");
      return;
    }
    g_camera = dev->getAddress();
    g_pairingOpen = (adv.mode22 & ADV_PAIRING_ENABLED) != 0;
    g_haveCamera = true;
    NimBLEDevice::getScan()->stop();
  }
};

static ScanCB g_scanCB;

// ---------------------------------------------------------------- Connect
static bool connectAndSubscribe() {
  g_ui = UiState::Connecting;
  g_command = nullptr;
  g_attempts++;

  const bool bonded = NimBLEDevice::isBonded(g_camera);
  Serial.printf("\n--- attempt %u: bonded=%d pairingScreenOpen=%d\n", g_attempts, bonded,
                g_pairingOpen);
  if (!bonded && !g_pairingOpen) {
    Serial.println("!! No bond stored AND the camera is not in pairing mode.");
    Serial.println("!! Go to MENU > Network > Bluetooth > Pairing on the camera and leave");
    Serial.println("!! that screen open. The camera will not accept a new bond otherwise.");
  }

  // Alternate IO capability every other attempt: DISPLAY_YESNO is what furble
  // uses for Sony, but NO_INPUT_OUTPUT (Just Works) is worth trying.
  const bool yesno = (g_attempts % 2) == 1;
  NimBLEDevice::setSecurityAuth(true, true, true);  // bonding, MITM, SC
  NimBLEDevice::setSecurityIOCap(yesno ? BLE_HS_IO_DISPLAY_YESNO : BLE_HS_IO_NO_INPUT_OUTPUT);
  Serial.printf("  iocap = %s\n", yesno ? "DISPLAY_YESNO" : "NO_INPUT_OUTPUT");

  NimBLEClient *client = NimBLEDevice::getClientByPeerAddress(g_camera);
  if (!client) {
    client = NimBLEDevice::createClient();
    client->setClientCallbacks(&g_clientCB, false);
  }
  client->setConnectTimeout(10000);
  client->setConnectionParams(12, 24, 0, 400);

  Serial.println("  connecting...");
  if (!client->connect(g_camera)) {
    Serial.println("  connect() failed");
    explainError(client->getLastError());
    NimBLEDevice::deleteClient(client);
    return false;
  }
  Serial.println("  connected, securing...");

  if (!client->secureConnection()) {
    Serial.println("  secureConnection() FAILED");
    explainError(client->getLastError());
    NimBLEConnInfo info = client->getConnInfo();
    Serial.printf("    conn state: encrypted=%d authenticated=%d bonded=%d\n", info.isEncrypted(),
                  info.isAuthenticated(), info.isBonded());
    client->disconnect();
    return false;
  }
  Serial.println("  secured");

  NimBLERemoteService *svc = client->getService(SVC_REMOTE);
  if (!svc) {
    Serial.println("  no 8000ff00 remote service -- is Bluetooth Rmt Ctrl On?");
    Serial.println("  services advertised by this camera:");
    for (auto *s : client->getServices(true)) {
      Serial.printf("    %s\n", s->getUUID().toString().c_str());
    }
    g_ui = UiState::NoService;
    return false;
  }

  NimBLERemoteCharacteristic *status = svc->getCharacteristic(CHR_STATUS);
  if (!status) {
    Serial.println("  no 0xff02 characteristic");
    client->disconnect();
    return false;
  }
  Serial.printf("  0xff02 props: notify=%d indicate=%d read=%d\n", status->canNotify(),
                status->canIndicate(), status->canRead());
  if (status->canRead()) {
    NimBLEAttValue v = status->readValue();
    Serial.print("  0xff02 initial read:");
    for (size_t i = 0; i < v.size(); i++) {
      Serial.printf(" %02x", v[i]);
    }
    Serial.println();
  }
  if (!status->subscribe(true, onStatus)) {
    Serial.println("  subscribe to 0xff02 failed");
    explainError(client->getLastError());
    client->disconnect();
    return false;
  }

  g_command = svc->getCharacteristic(CHR_COMMAND);
  Serial.printf("  0xff01 %s\n", g_command ? "found" : "MISSING");
  Serial.println("  ready -- waiting for status notifications");
  g_attempts = 0;
  g_ui = UiState::Connected;
  return true;
}

static void toggleRecord() {
  if (!g_command) {
    Serial.println("record toggle ignored: no command characteristic");
    return;
  }
  Serial.println("-> record toggle (01 0e / 01 0f)");
  g_command->writeValue(CMD_RECORD_DOWN, true);
  delay(50);
  g_command->writeValue(CMD_RECORD_UP, true);
}

// ---------------------------------------------------------------- Sketch
void setup() {
  Serial.begin(115200);
  delay(1500);  // let USB CDC come up so we don't lose the first lines

  pinMode(NEOPIXEL_POWER, OUTPUT);
  digitalWrite(NEOPIXEL_POWER, NEOPIXEL_POWER_ON);
  pixel.begin();
  pixel.clear();
  pixel.show();
  paint(UiState::Scanning);

  pinMode(BOOT_BTN, INPUT_PULLUP);

  Serial.println("\n=== Sony BLE tally ===");
  // This name is what the camera shows on its "pair with this device?" dialog.
  NimBLEDevice::init("QTPy Tally");
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);

  // REQUIRED for Sony: during pairing the camera reads OUR Device Name
  // (Generic Access 0x2A00) from our GATT server. NimBLE only exposes the GAP
  // service once a server is started; without it the read goes unanswered and
  // the camera drops the link with HCI 0x13 before SMP ever completes.
  // (Bluedroid always runs a server, which is why it "just works" there.)
  // See h2zero/NimBLE-Arduino#572; furble does the same in Scan.cpp.
  NimBLEServer *server = NimBLEDevice::createServer();
  server->start();
  Serial.println("GATT server started (GAP device name readable by camera)");
  NimBLEDevice::setSecurityInitKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);
  NimBLEDevice::setSecurityRespKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);

  printBonds();
  if (digitalRead(BOOT_BTN) == LOW) {
    Serial.println("BOOT held -> erasing all bonds");
    NimBLEDevice::deleteAllBonds();
    printBonds();
    while (digitalRead(BOOT_BTN) == LOW) {
      delay(10);
    }
  }
}

void loop() {
  const UiState want = g_ui;
  if (want != g_uiShown) {
    paint(want);
    g_uiShown = want;
  }

  static uint32_t lastPress = 0;
  if (digitalRead(BOOT_BTN) == LOW && millis() - lastPress > 500) {
    lastPress = millis();
    toggleRecord();
  }

  if (!g_haveCamera) {
    g_ui = UiState::Scanning;
    NimBLEScan *scan = NimBLEDevice::getScan();
    scan->setScanCallbacks(&g_scanCB, false);
    scan->setActiveScan(true);
    scan->setInterval(97);
    scan->setWindow(37);
    Serial.println("scanning for a Sony camera...");
    scan->getResults(5000, false);
    scan->clearResults();
    return;
  }

  NimBLEClient *client = NimBLEDevice::getClientByPeerAddress(g_camera);
  if (client && client->isConnected()) {
    delay(50);
    return;
  }

  if (!connectAndSubscribe()) {
    // Re-scan so we pick up a fresh mode22 (did the pairing screen open?)
    g_haveCamera = false;
    delay(2000);
  }
}