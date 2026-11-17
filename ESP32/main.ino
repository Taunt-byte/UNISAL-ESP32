/*
 * =====================================================================
 *  ESTAÇÃO METEOROLÓGICA COM PREVISÃO POR PRESSÃO ATMOSFÉRICA
 * =====================================================================
 *  Hardware:
 *    - ESP32 WROOM-32D (DevKit)
 *    - Sensor BME280 (temperatura, umidade e pressão) via I2C
 *    - Display OLED SSD1306 128x64 via I2C
 *
 *  Funcionamento:
 *    - Lê temperatura, umidade e pressão a cada INTERVALO_MIN minutos.
 *    - Guarda um histórico de pressão na NVS (memória não volátil),
 *      para calcular a variação das últimas 3 horas.
 *    - Classifica a tendência do tempo a partir dessa variação.
 *    - Envia os dados ao ThingSpeak quando há conexão Wi-Fi.
 *    - Se o BME280 não responder, mostra uma tela de erro animada e
 *      tenta de novo automaticamente.
 *
 *  Bibliotecas necessárias (Gerenciador de Bibliotecas da Arduino IDE):
 *    - Adafruit BME280 Library
 *    - Adafruit SSD1306
 *    - Adafruit GFX Library
 *    - Adafruit Unified Sensor
 *    (WiFi, HTTPClient, Preferences e Wire já vêm com o pacote ESP32)
 * =====================================================================
 */

#include <Wire.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_BME280.h>

// =====================================================================
//  CONFIGURAÇÕES DO USUÁRIO
// =====================================================================

// true  = modo economia (deep sleep entre leituras; o OLED fica
//         ligado só por alguns segundos a cada ciclo)
// false = modo contínuo (OLED sempre ligado; útil para testes)
#define MODO_ECONOMIA   false

// Dados da rede Wi-Fi (o ESP32 só conecta em redes 2.4 GHz)
const char* WIFI_SSID      = "SEU_WIFI";
const char* WIFI_SENHA     = "SUA_SENHA";

// Chave de escrita do canal no ThingSpeak
const char* THINGSPEAK_KEY = "SUA_WRITE_API_KEY";

// =====================================================================
//  HARDWARE
// =====================================================================

#define SCREEN_WIDTH   128      // largura do OLED em pixels
#define SCREEN_HEIGHT  64       // altura do OLED em pixels
#define OLED_RESET     -1       // -1: o módulo não usa pino de reset
#define OLED_ADDR      0x3C     // endereço I2C do OLED

#define BME_ADDR       0x76     // use 0x77 se o seu módulo usar esse endereço

#define SDA_PIN        21       // GPIO usado para SDA (dados I2C)
#define SCL_PIN        22       // GPIO usado para SCL (clock I2C)

// =====================================================================
//  PARÂMETROS DE MEDIÇÃO
// =====================================================================

#define INTERVALO_MIN          5        // minutos entre leituras
#define HIST_SIZE              36       // 36 amostras x 5 min = 3 horas
#define SAMPLE_INTERVAL_S      (INTERVALO_MIN * 60ULL)
#define TEMPO_EXIBE_OLED_MS    8000     // tempo com OLED ligado (modo economia)
#define INTERVALO_TENTATIVA_MS 2000     // pausa entre tentativas de achar o BME280

#define uS_TO_S                1000000ULL

// =====================================================================
//  OBJETOS GLOBAIS
// =====================================================================

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
Adafruit_BME280  bme;
Preferences      prefs;

bool sensorOK = false;   // indica se o BME280 foi inicializado com sucesso

// =====================================================================
//  HISTÓRICO DE PRESSÃO (NVS)
// =====================================================================

// Lê uma amostra de pressão do histórico, pelo índice
float lerHistorico(int i) {
  char chave[8];
  snprintf(chave, sizeof(chave), "p%d", i);
  return prefs.getFloat(chave, 0.0f);
}

// Grava uma amostra de pressão no histórico, pelo índice
void gravarHistorico(int i, float valor) {
  char chave[8];
  snprintf(chave, sizeof(chave), "p%d", i);
  prefs.putFloat(chave, valor);
}

// Retorna a pressão mais antiga disponível, que é a referência da tendência.
// Quando o histórico está cheio, a posição "idx" é a mais antiga,
// pois é a próxima a ser sobrescrita.
float pressaoMaisAntiga(int idxAtual, int count) {
  if (count < HIST_SIZE) return lerHistorico(0);
  return lerHistorico(idxAtual % HIST_SIZE);
}

// Classifica a tendência com base na variação de pressão (hPa) em 3 horas.
// Pressão subindo costuma indicar melhora; caindo rápido, chance de chuva.
const char* previsao(float deltaP) {
  if (deltaP >=  1.5) return "Melhorando";
  if (deltaP >=  0.5) return "Estavel+";
  if (deltaP >  -0.5) return "Estavel";
  if (deltaP >  -1.5) return "Instavel";
  return "Chuva";
}

// =====================================================================
//  TELA DE ERRO DO SENSOR
// =====================================================================

// Desenha a tela de erro do BME280 com um ícone de X, o número da
// tentativa e uma barra de progresso animada.
// 'quadro' avança a cada chamada e faz a barra se mover.
void telaErroSensor(uint8_t quadro, uint16_t tentativa) {
  display.clearDisplay();

  // Ícone: círculo com um X no centro
  display.drawCircle(20, 24, 14, SSD1306_WHITE);
  display.drawLine(13, 17, 27, 31, SSD1306_WHITE);
  display.drawLine(27, 17, 13, 31, SSD1306_WHITE);

  // Mensagens à direita do ícone
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(44, 8);
  display.println("BME280");
  display.setCursor(44, 20);
  display.println("nao achado");
  display.setCursor(44, 32);
  display.print("Tentativa ");
  display.println(tentativa);

  // Barra de progresso: um bloco que se desloca de um lado a outro
  display.drawRect(0, 54, 128, 8, SSD1306_WHITE);
  int16_t posicao = (quadro % 22) * 5;          // 0 a 105 pixels
  display.fillRect(2 + posicao, 56, 10, 4, SSD1306_WHITE);

  display.display();
}

// =====================================================================
//  TELA DE DADOS
// =====================================================================

// Mostra as leituras atuais e a classificação da tendência.
void mostrarDados(float temp, float hum, float pres, float deltaP) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(0, 0);
  display.print("Temp: "); display.print(temp, 1); display.println(" C");
  display.print("Umid: "); display.print(hum, 0);  display.println(" %");
  display.print("Pres: "); display.print(pres, 1); display.println(" hPa");
  display.println("---------------------");          // 21 traços = 126 px
  display.print("Var 3h: "); display.print(deltaP, 1); display.println(" hPa");

  // Classificação em destaque (tamanho 2)
  display.setTextSize(2);
  display.println(previsao(deltaP));

  display.display();
}

// =====================================================================
//  WI-FI E ENVIO AO THINGSPEAK
// =====================================================================

// Tenta conectar ao Wi-Fi por até 15 segundos.
// Retorna true se a conexão foi estabelecida.
bool conectarWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_SENHA);

  unsigned long inicio = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - inicio < 15000) {
    delay(250);
  }
  return WiFi.status() == WL_CONNECTED;
}

// Envia as quatro leituras para os campos 1 a 4 do canal ThingSpeak.
void enviarThingSpeak(float temp, float hum, float pres, float tendencia) {
  String url = "http://api.thingspeak.com/update?api_key=";
  url += THINGSPEAK_KEY;
  url += "&field1=" + String(temp, 2);
  url += "&field2=" + String(hum, 1);
  url += "&field3=" + String(pres, 2);
  url += "&field4=" + String(tendencia, 2);

  HTTPClient http;
  http.begin(url);
  int codigo = http.GET();          // 200 indica sucesso
  Serial.printf("ThingSpeak: HTTP %d\n", codigo);
  http.end();
}

// =====================================================================
//  INICIALIZAÇÃO
// =====================================================================

void setup() {
  Serial.begin(115200);
  delay(100);

  // Inicializa o barramento I2C nos pinos definidos
  Wire.begin(SDA_PIN, SCL_PIN);

  // OLED: se não responder, não há como mostrar erro, então para aqui
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("OLED nao encontrado. Verifique as ligacoes e o endereco.");
    while (true) delay(1000);
  }

  // Tela de abertura rápida
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(20, 28);
  display.println("Estacao iniciando");
  display.display();
  delay(1000);

  // BME280: tenta inicializar. Se falhar, mostra a tela de erro com
  // animação e tenta de novo. O programa só segue quando o sensor responde.
  uint16_t tentativa = 0;
  while (!sensorOK) {
    tentativa++;
    sensorOK = bme.begin(BME_ADDR, &Wire);
    if (sensorOK) break;

    Serial.printf("BME280 nao encontrado (tentativa %u)\n", tentativa);

    // Anima a tela de erro durante o intervalo entre tentativas
    uint8_t quadros = INTERVALO_TENTATIVA_MS / 400;
    for (uint8_t q = 0; q < quadros; q++) {
      telaErroSensor(q, tentativa);
      delay(400);
    }
  }
  Serial.println("BME280 encontrado com sucesso.");

  // Abre o armazenamento não volátil (namespace "estacao")
  prefs.begin("estacao", false);

  // 1) Lê os sensores
  float temp = bme.readTemperature();
  float hum  = bme.readHumidity();
  float pres = bme.readPressure() / 100.0F;   // Pa -> hPa

  // 2) Atualiza o histórico de pressão (buffer circular)
  int idx   = prefs.getInt("idx", 0);
  int count = prefs.getInt("count", 0);

  if (count == 0) {
    // Primeira execução: preenche todo o histórico com a pressão atual
    for (int i = 0; i < HIST_SIZE; i++) gravarHistorico(i, pres);
    count = 1;
    idx   = 1;
  } else {
    gravarHistorico(idx, pres);
    idx = (idx + 1) % HIST_SIZE;
    if (count < HIST_SIZE) count++;
  }
  prefs.putInt("idx", idx);
  prefs.putInt("count", count);

  // 3) Calcula a variação de pressão em relação à amostra mais antiga
  float antiga = pressaoMaisAntiga(idx, count);
  float deltaP = pres - antiga;

  // 4) Mostra os dados no OLED
  mostrarDados(temp, hum, pres, deltaP);

  // 5) Envia ao ThingSpeak, se houver Wi-Fi
  if (conectarWiFi()) {
    enviarThingSpeak(temp, hum, pres, deltaP);
    WiFi.disconnect(true);
  } else {
    Serial.println("Wi-Fi nao conectou. Dados mostrados apenas no OLED.");
  }

  prefs.end();

#if MODO_ECONOMIA
  // 6) Mantém o OLED ligado por um tempo e depois entra em deep sleep
  delay(TEMPO_EXIBE_OLED_MS);
  display.clearDisplay();
  display.display();
  display.ssd1306_command(SSD1306_DISPLAYOFF);   // desliga o OLED

  esp_sleep_enable_timer_wakeup(SAMPLE_INTERVAL_S * uS_TO_S);
  esp_deep_sleep_start();                        // não retorna daqui
#endif
}

// =====================================================================
//  LOOP
// =====================================================================

void loop() {
#if !MODO_ECONOMIA
  // Modo contínuo: a cada intervalo, reinicia o ciclo.
  // O histórico está na NVS, então a tendência é preservada.
  static unsigned long ultimaLeitura = 0;

  if (millis() - ultimaLeitura >= SAMPLE_INTERVAL_S * 1000UL) {
    ultimaLeitura = millis();
    ESP.restart();
  }
  delay(1000);
#endif
}
