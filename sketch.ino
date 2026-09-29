// Sensor de sala — ESP32 (Wokwi) — PIR + LDR + LED + Cloudflare Worker/D1
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

// ---------- Configuração ----------
const char* WORKER_URL = "https://sensor-sala.gabee-roggia.workers.dev";

const int PIN_PIR = 27;
const int PIN_LDR = 34;
const int PIN_LED = 2;

// R1 — Calibração do LDR (valores anotados no Serial Monitor do Wokwi):
//   luz ALTA  (100000 lux) -> analogRead(34) = 32
//   luz média (500 lux)    -> analogRead(34) = 1001
//   luz BAIXA (0.1 lux)    -> analogRead(34) = 4063
//   No módulo LDR, MAIS luz = MENOR leitura. Limiar = ponto médio (32 + 4063) / 2 ~ 2000.
const int  LIMIAR_LUZ       = 2000;
const bool LUZ_ACIMA_LIMIAR = false;  // false: leitura < limiar = luz acesa

const unsigned long TEMPO_OCUPADA_MS  = 30000;  // R3: ocupada até 30 s após o último movimento
const unsigned long TEMPO_ALERTA_MS   = 60000;  // R3: vazia + luz acesa por > 60 s = desperdício
const unsigned long INTERVALO_LUZ_MS  = 20000;  // R4: POST /luz a cada 20 s
const unsigned long PISCA_MS          = 250;

// ---------- Variáveis da interrupção (R2) ----------
volatile uint32_t movPendentes = 0;        // movimentos ainda não enviados
volatile unsigned long ultimoMovMs = 0;    // millis() do último movimento
volatile bool houveMovimento = false;

void IRAM_ATTR onMovimento() {
  movPendentes++;
  ultimoMovMs = millis();
  houveMovimento = true;
}

// ---------- Estado ----------
uint32_t movEnviados = 0;        // movimentos já gravados no D1
uint32_t totalVisto = 0;        // total acumulado já mostrado no Serial
unsigned long ultimoEnvioLuz = 0;
unsigned long vaziaComLuzDesde = 0;   // 0 = não está vazia com luz acesa
unsigned long ultimoPisca = 0;
bool ledPisca = false;
int leituraLuz = 0;
bool luzAcesa = false;

// Estados da sala
const int VAZIA = 0;
const int OCUPADA = 1;
const int DESPERDICIO = 2;
int estado = VAZIA;
int estadoAnterior = -1;

// ---------- Utilitários ----------
const char* nomeEstado(int e) {
  switch (e) {
    case OCUPADA:     return "OCUPADA";
    case DESPERDICIO: return "DESPERDICIO (vazia com luz acesa > 60 s)";
    default:          return "VAZIA";
  }
}

bool lerLuz() {
  leituraLuz = analogRead(PIN_LDR);
  luzAcesa = LUZ_ACIMA_LIMIAR ? (leituraLuz > LIMIAR_LUZ) : (leituraLuz < LIMIAR_LUZ);
  return luzAcesa;
}

void conectarWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  Serial.print("[WiFi] Conectando");
  WiFi.begin("Wokwi-GUEST", "", 6);
  while (WiFi.status() != WL_CONNECTED) {
    delay(250);
    Serial.print(".");
  }
  Serial.print(" conectado! IP: ");
  Serial.println(WiFi.localIP());
}

int enviarPost(const char* rota, const String& corpo) {
  if (WiFi.status() != WL_CONNECTED) conectarWiFi();
  WiFiClientSecure client;    // conexão TLS nova a cada envio (mais estável no Wokwi)
  client.setInsecure();       // HTTPS (Cloudflare) sem validar certificado
  HTTPClient http;
  http.setReuse(false);
  http.setTimeout(10000);
  if (!http.begin(client, String(WORKER_URL) + rota)) return -100;
  http.addHeader("Content-Type", "application/json");
  int codigo = http.POST(corpo);
  http.end();
  return codigo;
}

// ---------- Setup ----------
void setup() {
  Serial.begin(115200);
  pinMode(PIN_PIR, INPUT);
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);
  analogReadResolution(12);   // 0 a 4095

  conectarWiFi();

  attachInterrupt(digitalPinToInterrupt(PIN_PIR), onMovimento, RISING);

  lerLuz();
  Serial.println("==== Sensor de sala iniciado ====");
  Serial.printf("[LUZ] leitura inicial = %d | LIMIAR_LUZ = %d | %s\n",
                leituraLuz, LIMIAR_LUZ, luzAcesa ? "ACESA" : "APAGADA");
}

// ---------- Loop ----------
void loop() {
  // Cópia segura das variáveis da ISR
  noInterrupts();
  uint32_t pendentes = movPendentes;
  unsigned long ultimoMov = ultimoMovMs;
  bool jaHouveMov = houveMovimento;
  interrupts();

  unsigned long agora = millis();
  lerLuz();   // leitura local para o LED reagir rápido

  // R3 — estado da sala
  bool ocupada = jaHouveMov && (agora - ultimoMov < TEMPO_OCUPADA_MS);

  if (!ocupada && luzAcesa) {
    if (vaziaComLuzDesde == 0) vaziaComLuzDesde = agora;
  } else {
    vaziaComLuzDesde = 0;
  }

  if (ocupada) estado = OCUPADA;
  else if (vaziaComLuzDesde != 0 && agora - vaziaComLuzDesde > TEMPO_ALERTA_MS) estado = DESPERDICIO;
  else estado = VAZIA;

  if (estado != estadoAnterior) {
    Serial.printf("[SALA] estado: %s\n", nomeEstado(estado));
    estadoAnterior = estado;
  }

  // LED: aceso = ocupada | piscando = desperdício | apagado = vazia
  if (estado == OCUPADA) {
    digitalWrite(PIN_LED, HIGH);
  } else if (estado == DESPERDICIO) {
    if (agora - ultimoPisca >= PISCA_MS) {
      ultimoPisca = agora;
      ledPisca = !ledPisca;
      digitalWrite(PIN_LED, ledPisca);
    }
  } else {
    digitalWrite(PIN_LED, LOW);
  }

  // ---- Envios (depois de atualizar o LED, para o aviso visual ser imediato) ----
  // R4 — um POST /movimento para cada movimento (só desconta após sucesso: nada se perde)
  uint32_t totalDetectado = movEnviados + pendentes;
  while (totalVisto < totalDetectado) {
    totalVisto++;
    Serial.printf("[MOVIMENTO] detectado! total acumulado = %lu\n", (unsigned long)totalVisto);
  }

  if (pendentes > 0) {
    int codigo = enviarPost("/movimento", "{}");
    Serial.printf("[HTTP] POST /movimento -> %d (restam na fila: %lu)\n", codigo,
                  (unsigned long)(codigo == 201 ? pendentes - 1 : pendentes));
    if (codigo == 201) {
      noInterrupts();
      movPendentes--;       // só sai da fila depois de gravado no D1
      interrupts();
      movEnviados++;
    } else {
      Serial.println("[HTTP] falha no envio, movimento mantido na fila para reenvio");
      delay(1000);
    }
  }

  // R4 — POST /luz a cada 20 s
  if (agora - ultimoEnvioLuz >= INTERVALO_LUZ_MS || ultimoEnvioLuz == 0) {
    ultimoEnvioLuz = agora;
    Serial.printf("[LUZ] leitura = %d -> %s (limiar %d)\n",
                  leituraLuz, luzAcesa ? "ACESA" : "APAGADA", LIMIAR_LUZ);
    String corpo = "{\"valor\":" + String(leituraLuz) + ",\"acesa\":" + String(luzAcesa ? 1 : 0) + "}";
    int codigo = enviarPost("/luz", corpo);
    Serial.printf("[HTTP] POST /luz %s -> %d\n", corpo.c_str(), codigo);
  }

  delay(10);
}
