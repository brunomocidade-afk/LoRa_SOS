#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <TinyGPS++.h>
#include <math.h>

#define SCREEN_WIDTH 128 
#define SCREEN_HEIGHT 64 

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// Instância do Parser de GPS
TinyGPSPlus gps;

// Pinos Seguros de Hardware
#define BUTTON_PIN 25      // Botão de pânico local (GPIO 25 e GND)
#define LED_PANIC_PIN 14   // LED vermelho de emergência
#define LED_AMARELO_PIN 26 // LED amarelo externo para pacotes comuns (PING/PONG)

// Rádio REYAX RYLR998 (UART2)
#define RX2_PIN 16         // Conectado ao TXD do REYAX
#define TX2_PIN 17         // Conectado ao RXD do REYAX

// Módulo GPS Neo-6M (UART1 - Lado Esquerdo)
#define GPS_RX_PIN 32      // Conectado ao TX do GPS Neo-6M
#define GPS_TX_PIN 33      // Conectado ao RX do GPS Neo-6M (opcional)

// Endereço da Estação Repetidora
#define ENDERECO_LORA 11

// =========================================================================
// CONFIGURAÇÕES DE REDE E TELEGRAM
// =========================================================================
const char* WIFI_SSID = "xxxxx";
const char* WIFI_PASSWORD = "xxxxxxx";

const String TELEGRAM_TOKEN = "coloque_aqui_seu_token"; 
const String TELEGRAM_CHAT_ID = "coloque_aqui_seu_chat_id"; 

// LOCALIZAÇÃO FIXA DE FALLBACK (Araraquara SP - usada caso o GPS esteja sem fix)
String latAtualRepetidora = "-21.79461";
String lngAtualRepetidora = "-48.11121";
bool gpsSincronizado = false;
// =========================================================================

volatile bool panicPressed = false;
unsigned long lastDebounceTime = 0;
const unsigned long debounceDelay = 250;

// Estado global de alerta e métricas
bool emEstadoDePanico = false;
String panicIdExibicao = "";
String panicLatExibicao = "";
String panicLngExibicao = "";
int panicHopsExibicao = 0;

unsigned long contadorPacotes = 0; // Contador sequencial de pacotes PING recebidos

// Função Haversine com ALTA PRECISÃO (double de 64 bits)
double calcularDistanciaHaversine(double lat1, double lon1, double lat2, double lon2) {
  const double R = 6371000.0; // Raio médio da Terra em metros
  double radLat1 = lat1 * M_PI / 180.0;
  double radLon1 = lon1 * M_PI / 180.0;
  double radLat2 = lat2 * M_PI / 180.0;
  double radLon2 = lon2 * M_PI / 180.0;

  double dLat = radLat2 - radLat1;
  double dLon = radLon2 - radLon1;

  double a = sin(dLat / 2.0) * sin(dLat / 2.0) +
             cos(radLat1) * cos(radLat2) *
             sin(dLon / 2.0) * sin(dLon / 2.0);

  double c = 2.0 * atan2(sqrt(a), sqrt(1.0 - a));
  return R * c;
}

void IRAM_ATTR handlePanicButton() {
  unsigned long currentTime = millis();
  if (currentTime - lastDebounceTime > debounceDelay) {
    panicPressed = true;
    lastDebounceTime = currentTime;
  }
}

void setup() {
  Serial.begin(115200);
  
  pinMode(LED_PANIC_PIN, OUTPUT);
  digitalWrite(LED_PANIC_PIN, LOW);
  
  pinMode(LED_AMARELO_PIN, OUTPUT);
  digitalWrite(LED_AMARELO_PIN, LOW);

  // Teste de inicialização: Pisca o LED Amarelo 2 vezes ao ligar
  digitalWrite(LED_AMARELO_PIN, HIGH);
  delay(150);
  digitalWrite(LED_AMARELO_PIN, LOW);
  delay(150);
  digitalWrite(LED_AMARELO_PIN, HIGH);
  delay(150);
  digitalWrite(LED_AMARELO_PIN, LOW);
  
  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println(F("Falha ao inicializar o display SSD1306"));
  }
  
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 10);
  display.println("REPETIDORA LoRa");
  display.println("Iniciando GPS Neo-6M...");
  display.display();

  // Inicializa UART1 para o GPS (Baudrate padrão do Neo-6M - GPS é 9600)
  Serial1.begin(9600, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);

  // Inicializa Wi-Fi
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long startAttemptTime = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < 4000) {
    delay(500);
    Serial.print(".");
  }
  
  if(WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWi-Fi Conectado com sucesso!");
  } else {
    Serial.println("\nWi-Fi nao encontrado. Modo Offline.");
  }
  
  // Inicializa UART2 para o Rádio LoRa REYAX
  Serial2.begin(115200, SERIAL_8N1, RX2_PIN, TX2_PIN);
  delay(500);

  Serial2.println("AT+NETWORKID=10");
  delay(100);
  Serial2.println("AT+ADDRESS=" + String(ENDERECO_LORA));
  delay(100);

  // Configuração de Desempenho Máximo (compatível com Kotlin)
  Serial2.println("AT+PARAMETER=11,7,4,12"); 
  delay(100);
  Serial2.println("AT+CRFOP=22"); 
  delay(100);
  
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), handlePanicButton, FALLING);

  delay(1000);
  atualizarDisplayNormal("Em espera...", "N/A", "N/A");
}

void loop() {
  // Fluxograma Principal
  // 1. Processa dados recebidos do Módulo GPS continuamente
  while (Serial1.available() > 0) {
    gps.encode(Serial1.read());
  }

  // Verifica se o GPS adquiriu sinal válido de satélites
  if (gps.location.isValid() && gps.location.isUpdated()) {
    gpsSincronizado = true;
    latAtualRepetidora = String(gps.location.lat(), 5);
    lngAtualRepetidora = String(gps.location.lng(), 5);
  }

  // 2. Verificação do Botão de Emergência Local
  if (panicPressed) {
    panicPressed = false;
    sendPanicMessage();
  }

  // 3. Processamento de mensagens do Rádio LoRa
  if (Serial2.available()) {
    String receivedData = Serial2.readStringUntil('\n');
    receivedData.trim();
    
    if (receivedData.length() > 0) {
      Serial.print("[LoRa Recebido]: ");
      Serial.println(receivedData);
      
      if (receivedData.startsWith("+RCV=")) {
        parseAndRepeat(receivedData);
      }
    }
  }
}

void sendPanicMessage() {
  Serial.println("[ALERTA LOCAL] Botao pressionado! Disparando SOS...");
  
  digitalWrite(LED_PANIC_PIN, HIGH);
  emEstadoDePanico = true;
  
  panicIdExibicao = "LOCAL (" + String(ENDERECO_LORA) + ")";
  panicLatExibicao = latAtualRepetidora;
  panicLngExibicao = lngAtualRepetidora;
  panicHopsExibicao = 0;
  
  atualizarDisplayPanico(panicIdExibicao, panicLatExibicao, panicLngExibicao, panicHopsExibicao);

  String msgPayload = "SOS;" + latAtualRepetidora + ";" + lngAtualRepetidora + ";0";
  String cmd = "AT+SEND=0," + String(msgPayload.length()) + "," + msgPayload;
  Serial2.println(cmd);
  
  enviarMensagemTelegram(panicIdExibicao, panicLatExibicao, panicLngExibicao, panicHopsExibicao);
}

void parseAndRepeat(String rcvLine) {
  int equalSign = rcvLine.indexOf('=');
  int firstComma = rcvLine.indexOf(',');
  int secondComma = rcvLine.indexOf(',', firstComma + 1);
  int thirdComma = rcvLine.indexOf(',', secondComma + 1);
  
  if (equalSign != -1 && firstComma != -1 && secondComma != -1 && thirdComma != -1) {
    String senderID = rcvLine.substring(equalSign + 1, firstComma);
    String msgData = rcvLine.substring(secondComma + 1, thirdComma);
    msgData.trim();
    
    String tipoMsg = msgData;
    String lat = "N/A";
    String lon = "N/A";
    int hopsInt = 0;
    
    int p1 = msgData.indexOf(';');
    int p2 = msgData.indexOf(';', p1 + 1);
    int p3 = msgData.indexOf(';', p2 + 1);
    
    if (p1 != -1) {
      tipoMsg = msgData.substring(0, p1);
      if (p2 != -1) {
        lat = msgData.substring(p1 + 1, p2);
        if (p3 != -1) {
          lon = msgData.substring(p2 + 1, p3);
          hopsInt = msgData.substring(p3 + 1).toInt();
        } else {
          lon = msgData.substring(p2 + 1);
        }
      } else {
        lat = msgData.substring(p1 + 1);
      }
    }
    
    int novosHops = hopsInt + 1;
    String novoPayload = tipoMsg + ";" + lat + ";" + lon + ";" + String(novosHops);
    
    // Cálculo de Distância em metros entre o Emissor e o GPS da Repetidora
    String textoDistancia = "N/A";
    if (lat != "N/A" && lon != "N/A" && lat != "0.00000" && lon != "0.00000") {
      double dLatEmissor = atof(lat.c_str());
      double dLonEmissor = atof(lon.c_str());
      double dLatRepetidora = atof(latAtualRepetidora.c_str());
      double dLonRepetidora = atof(lngAtualRepetidora.c_str());

      double distMetros = calcularDistanciaHaversine(dLatEmissor, dLonEmissor, dLatRepetidora, dLonRepetidora);
      textoDistancia = String(distMetros, 1) + " m";
    }

    // --- CASO A MENSAGEM SEJA UM SOS ---
    if (tipoMsg.equals("SOS") || tipoMsg.startsWith("SOS")) {
      Serial.println("[ALERTA REMOTO] SOS recebido do ID: " + senderID);
      
      digitalWrite(LED_PANIC_PIN, HIGH);
      emEstadoDePanico = true;
      
      panicIdExibicao = senderID;
      panicLatExibicao = lat;
      panicLngExibicao = lon;
      panicHopsExibicao = novosHops;
      
      atualizarDisplayPanico(panicIdExibicao, panicLatExibicao, panicLngExibicao, panicHopsExibicao);
      
      String cmd = "AT+SEND=0," + String(novoPayload.length()) + "," + novoPayload;
      Serial2.println(cmd);
      
      enviarMensagemTelegram(panicIdExibicao, panicLatExibicao, panicLngExibicao, panicHopsExibicao);
    } 
    // --- CASO SEJA UM PACOTE COMUM (PING, PONG, ETC.) ---
    else {
      if (tipoMsg.equals("PING")) {
        contadorPacotes++;
      }

      char numPacoteStr[5];
      snprintf(numPacoteStr, sizeof(numPacoteStr), "%03lu", contadorPacotes);

      Serial.println("-> Retransmitindo pacote [" + tipoMsg + "] do ID: " + senderID + " | Nro: " + String(numPacoteStr) + " | Dist: " + textoDistancia);
      
      digitalWrite(LED_AMARELO_PIN, HIGH);
      
      if (!emEstadoDePanico) {
        atualizarDisplayNormal("Repetindo ID " + senderID, tipoMsg + " (ID " + senderID + ") " + String(numPacoteStr), textoDistancia);
      }
      
      // Responde PONG injetando as coordenadas do GPS da Repetidora e o contador
      String payloadEnvio = novoPayload;
      if (tipoMsg.equals("PONG")) {
        payloadEnvio = "PONG;" + latAtualRepetidora + ";" + lngAtualRepetidora + ";" + String(numPacoteStr);
      }

      String cmd = "AT+SEND=0," + String(payloadEnvio.length()) + "," + payloadEnvio;
      Serial2.println(cmd);
      
      delay(150); 
      digitalWrite(LED_AMARELO_PIN, LOW);
      
      if (!emEstadoDePanico) {
        atualizarDisplayNormal("Em espera...", tipoMsg + " (ID " + senderID + ") " + String(numPacoteStr), textoDistancia);
      }
    }
  }
}

void enviarMensagemTelegram(String idOrigem, String lat, String lng, int hops) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[TELEGRAM] Abortado: Sem conexao Wi-Fi no momento.");
    return;
  }
  
  Serial.println("[TELEGRAM] Enviando alerta para o grupo...");
  HTTPClient http;
  
  String url = "https://api.telegram.org/bot" + TELEGRAM_TOKEN + "/sendMessage";
  
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  
  String textoMensagem = "🚨 *ALERTA DE EMERGENCIA SOS* 🚨\n\n";
  textoMensagem += "*Origem:* Modulo ID " + idOrigem + "\n";
  textoMensagem += "*Saltos (Hops):* " + String(hops) + "\n";
  textoMensagem += "*Latitude:* " + lat + "\n";
  textoMensagem += "*Longitude:* " + lng + "\n\n";
  
  if (lat != "N/A" && lat != "Desconhecida" && lat != "Nao enviada") {
    textoMensagem += "📍 [Ver localizacao no Google Maps](https://www.google.com/maps/search/?api=1&query=" + lat + "," + lng + ")";
  }
  
  String jsonPayload = "{\"chat_id\": \"" + TELEGRAM_CHAT_ID + "\", \"text\": \"" + textoMensagem + "\", \"parse_mode\": \"Markdown\"}";
  
  int httpResponseCode = http.POST(jsonPayload);
  
  if (httpResponseCode > 0) {
    Serial.print("[TELEGRAM] Sucesso! Resposta HTTP: ");
    Serial.println(httpResponseCode);
  } else {
    Serial.print("[TELEGRAM] Erro no envio. Codigo HTTP: ");
    Serial.println(httpResponseCode);
  }
  
  http.end();
}

void atualizarDisplayNormal(String statusLinha, String infoPacote, String distancia) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  
  // Linha 1: (ID:11) W:OK G:OK ou (ID:11) W:OK G:---
  display.setCursor(0, 0);
  display.print("(ID:");
  display.print(ENDERECO_LORA);
  display.print(") W:");
  if (WiFi.status() == WL_CONNECTED) display.print("OK");
  else display.print("OFF");
  
  display.print(" G:");
  if (gpsSincronizado) display.println("OK");
  else display.println("---");
  
  display.println("---------------------");
  
  // Linha 2: PING (ID 3) 001
  display.setCursor(0, 18);
  if (infoPacote != "N/A") {
    display.println(infoPacote);
  } else {
    display.println("Aguardando trafego...");
  }
  
  // Linha 3: Dist: 4.9 m
  display.setCursor(0, 32);
  display.print("Dist: ");
  display.println(distancia);

  // Linha 4 (Rodapé): Status da Repetidora
  display.setCursor(0, 48);
  display.print("Status: ");
  display.println(statusLinha);
  
  display.display();
}

void atualizarDisplayPanico(String id, String lat, String lng, int hops) {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("!! ALERTA CRITICO !!");
  display.println("---------------------");
  display.setCursor(0, 18);
  display.print("SOS Origem: ID "); display.println(id);
  display.setCursor(0, 30);
  display.print("Saltos (Hops): "); display.println(hops);
  display.setCursor(0, 42);
  display.print("Lat: "); display.println(lat);
  display.setCursor(0, 54);
  display.print("Lng: "); display.println(lng);
  display.display();
}
