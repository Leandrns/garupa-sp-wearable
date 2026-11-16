#include <Wire.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <time.h>
#include "config.h"

#define MODO_API (sizeof(API_BASE_URL) > 1)

// ---------- Tipos ----------

enum Estado : uint8_t { MONITORANDO, IMPACTO_CANDIDATO, VERIFICANDO_IMOBILIDADE, CONTAGEM, SOS_ENVIADO };
const char *NOME_ESTADO[] = {"monitorando", "impacto_candidato", "verificando_imobilidade", "contagem", "sos_enviado"};

enum Alerta : uint8_t {
  A_NENHUM, A_TURNO_ATIVO, A_PONTO_CRITICO, A_MENSAGEM, A_CONTAGEM_QUEDA,
  A_SOS_ENVIADO, A_CANCELADO, A_TURNO_ENCERRADO, A_SEM_CONEXAO
};
// Os nomes coincidem com o campo "tipo" da API; sem_conexao é só do próprio vestível.
const char *NOME_ALERTA[] = {"nenhum", "turno_ativo", "ponto_critico", "mensagem", "contagem_queda",
                             "sos_enviado", "cancelado", "turno_encerrado", "sem_conexao"};

struct Rgb { uint8_t r, g, b; };
const Rgb DESLIGADO = {0, 0, 0};
const Rgb VERDE = {0, 255, 0};
const Rgb AMBAR = {255, 90, 0};
const Rgb AZUL = {0, 0, 255};
const Rgb VERMELHO = {255, 0, 0};
const Rgb BRANCO = {255, 255, 255};

struct Efeito {
  Alerta tipo = A_NENHUM;
  unsigned long inicio = 0;
};

struct EventoFila {
  char tipo[10];
  unsigned long tMs;
};

// ---------- Estado global ----------

Estado estado = MONITORANDO;
unsigned long tEstado = 0;
unsigned long tContagem = 0;
unsigned long tParadoDesde = 0;

bool contagemExterna = false;  // contagem exibida por alerta da API, sem contagem própria
bool modoTurno = false;
volatile bool semConexao = false;  // escrita também pela tarefa de rede
#if MODO_TESTE
bool debugImu = false;
#endif

Efeito efeito;
unsigned long tUltimoTurno = 0;
unsigned long tUltimoSemConexao = 0;

QueueHandle_t filaEventos = nullptr;
QueueHandle_t filaAlertas = nullptr;

WiFiClientSecure clienteTls;
WiFiClient clientePlano;
HTTPClient http;
int falhasSeguidas = 0;
int ultimoAviso = 0;
String tokenApi;
unsigned long proximaAutenticacao = 0;

// ---------- Rede ----------

bool horaSincronizada() { return time(nullptr) > 1700000000; }

// A API exige ocorridoEm em UTC com "Z" e recusa horário no futuro.
bool formatarHorario(unsigned long tEventoMs, char *saida, size_t tamanho) {
  time_t agora = time(nullptr);
  if (agora < 1700000000) return false;
  time_t t = agora - (millis() - tEventoMs) / 1000 - MARGEM_RELOGIO_S;
  struct tm tm;
  gmtime_r(&t, &tm);
  strftime(saida, tamanho, "%Y-%m-%dT%H:%M:%SZ", &tm);
  return true;
}

// Qualquer resposta abaixo de 500 prova que a API está acessível.
void registrarResultadoRede(int codigo) {
  if (codigo > 0 && codigo < 500) {
    if (semConexao) Serial.println("[REDE] conexao com a API restabelecida");
    falhasSeguidas = 0;
    semConexao = false;
    return;
  }
  falhasSeguidas++;
  if (falhasSeguidas >= FALHAS_PARA_SEM_CONEXAO && !semConexao) {
    Serial.println("[REDE] sem conexao com a API");
    semConexao = true;
  }
}

bool iniciarRequisicao(const String &url, bool autenticada) {
  bool seguro = strncmp(API_BASE_URL, "https", 5) == 0;
  WiFiClient *cliente = seguro ? (WiFiClient *)&clienteTls : &clientePlano;
  http.setReuse(true);  // evita refazer o handshake TLS a cada consulta
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.setConnectTimeout(HTTP_CONEXAO_MS);
  if (!http.begin(*cliente, url)) return false;
  if (autenticada) http.addHeader("Authorization", "Bearer " + tokenApi);
  return true;
}

void descartarToken() {
  tokenApi = "";
  proximaAutenticacao = 0;
  Serial.println("[REDE] token recusado (401), autenticando de novo");
}

// Retorna o código HTTP do login (ou -1 sem resposta). O token fica em tokenApi.
int autenticar() {
  if (!iniciarRequisicao(String(API_BASE_URL) + "/autenticacao/login", false)) {
    registrarResultadoRede(-1);
    return -1;
  }
  http.addHeader("Content-Type", "application/json");
  char corpo[160];
  snprintf(corpo, sizeof(corpo), "{\"telefone\":\"%s\",\"senha\":\"%s\"}", API_TELEFONE, API_SENHA);
  int codigo = http.POST(corpo);
  String resposta = codigo > 0 ? http.getString() : "";
  http.end();
  registrarResultadoRede(codigo);

  if (codigo == 200) {
    int chave = resposta.indexOf("\"token\"");
    int abre = chave < 0 ? -1 : resposta.indexOf('"', resposta.indexOf(':', chave) + 1);
    int fecha = abre < 0 ? -1 : resposta.indexOf('"', abre + 1);
    if (fecha > 0) {
      tokenApi = resposta.substring(abre + 1, fecha);
      Serial.println("[REDE] autenticado");
    } else {
      Serial.println("[REDE] resposta de login sem token");
    }
  } else if (codigo == 401) {
    Serial.println("[REDE] login recusado (401): confira API_TELEFONE e API_SENHA");
  } else if (codigo == 429) {
    Serial.println("[REDE] limite de tentativas de login excedido (429)");
  } else {
    Serial.printf("[REDE] login falhou (codigo %d)\n", codigo);
  }
  return codigo;
}

// Retorna true quando o evento terminou (enviado ou recusado); false para tentar de novo.
bool enviarEventoHttp(const EventoFila &ev) {
  char horario[25];
  if (!formatarHorario(ev.tMs, horario, sizeof(horario))) return false;
  char corpo[160];
  snprintf(corpo, sizeof(corpo), "{\"dispositivoId\":\"%s\",\"tipo\":\"%s\",\"ocorridoEm\":\"%s\"}",
           DISPOSITIVO_ID, ev.tipo, horario);
  if (!iniciarRequisicao(String(API_BASE_URL) + "/eventos-vestivel", true)) {
    registrarResultadoRede(-1);
    return false;
  }
  http.addHeader("Content-Type", "application/json");
  int codigo = http.POST(corpo);
  String resposta = codigo > 0 ? http.getString() : "";
  http.end();
  registrarResultadoRede(codigo);

  if (codigo == 202) {
    Serial.printf("[REDE] evento %s registrado (202)\n", ev.tipo);
    return true;
  }
  if (codigo == 401) {  // token expirado: o evento fica na fila até o novo login
    descartarToken();
    return false;
  }
  if (codigo > 0 && codigo < 500) {
    Serial.printf("[REDE] evento %s recusado (%d), descartado: %s\n", ev.tipo, codigo, resposta.c_str());
    return true;  // 4xx não se resolve repetindo
  }
  Serial.printf("[REDE] evento %s sem confirmacao (codigo %d), tentando de novo\n", ev.tipo, codigo);
  return false;
}

Alerta alertaDeTexto(const char *texto) {
  for (int i = A_TURNO_ATIVO; i <= A_TURNO_ENCERRADO; i++) {
    if (strcmp(texto, NOME_ALERTA[i]) == 0) return (Alerta)i;
  }
  return A_NENHUM;
}

void extrairAlertas(const String &json) {
  int pos = 0;
  while (true) {
    pos = json.indexOf("\"tipo\"", pos);
    if (pos < 0) break;
    int doisPontos = json.indexOf(':', pos);
    if (doisPontos < 0) break;
    int abre = json.indexOf('"', doisPontos + 1);
    if (abre < 0) break;
    int fecha = json.indexOf('"', abre + 1);
    if (fecha < 0) break;
    String tipo = json.substring(abre + 1, fecha);
    pos = fecha + 1;
    Alerta a = alertaDeTexto(tipo.c_str());
    if (a == A_NENHUM) {
      Serial.printf("[REDE] alerta desconhecido ignorado: %s\n", tipo.c_str());
    } else if (xQueueSend(filaAlertas, &a, 0) != pdTRUE) {
      Serial.println("[REDE] fila de alertas cheia: alerta descartado");
    }
  }
}

void consultarAlertas() {
  if (!iniciarRequisicao(String(API_BASE_URL) + "/vestivel/" + DISPOSITIVO_ID + "/alertas/pendentes", true)) {
    registrarResultadoRede(-1);
    return;
  }
  int codigo = http.GET();
  String corpo = codigo > 0 ? http.getString() : "";
  http.end();
  registrarResultadoRede(codigo);

  if (codigo == 401) {
    descartarToken();
    return;
  }
  if (codigo == 404 || codigo == 403) {  // a rede está boa, então não acende o branco
    if (ultimoAviso != codigo) {
      ultimoAviso = codigo;
      if (codigo == 404) Serial.println("[REDE] dispositivo nao pareado (404): pareie pelo app ou pelo Swagger");
      else Serial.println("[REDE] sem permissao (403): o login e o pareamento devem ser do mesmo usuario");
    }
    return;
  }
  if (codigo != 200) return;
  ultimoAviso = 0;
  extrairAlertas(corpo);
}

void tarefaRede(void *) {
  if (sizeof(API_TELEFONE) <= 1 || sizeof(API_SENHA) <= 1) {
    Serial.println("[REDE] preencha API_TELEFONE e API_SENHA no config.h");
    vTaskDelete(nullptr);
  }
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, "", WIFI_CANAL);
  clienteTls.setInsecure();  // simulador; em produção, validar o certificado da API
  configTime(0, 0, NTP_SERVIDOR_1, NTP_SERVIDOR_2);
  Serial.println("[REDE] conectando ao WiFi...");

  unsigned long proximaConsulta = 0;
  unsigned long proximoEnvio = 0;
  bool wifiAvisado = false;

  for (;;) {
    unsigned long agora = millis();

    if (WiFi.status() != WL_CONNECTED) {
      wifiAvisado = false;
      if (agora >= proximaConsulta) {
        proximaConsulta = agora + CONSULTA_ALERTAS_MS;
        registrarResultadoRede(-1);
      }
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }
    if (!wifiAvisado) {
      wifiAvisado = true;
      Serial.println("[REDE] WiFi conectado");
    }

    if (tokenApi.isEmpty()) {
      if (agora >= proximaAutenticacao) {
        int codigo = autenticar();
        unsigned long espera = codigo == 429 ? ESPERA_LIMITE_LOGIN_MS
                             : codigo == 401 ? ESPERA_LOGIN_RECUSADO_MS
                             : ESPERA_LOGIN_FALHO_MS;
        proximaAutenticacao = millis() + espera;
      }
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    // Eventos têm prioridade sobre a consulta de alertas.
    EventoFila ev;
    while (agora >= proximoEnvio && xQueuePeek(filaEventos, &ev, 0) == pdTRUE) {
      if (!horaSincronizada()) break;
      if (!enviarEventoHttp(ev)) {
        proximoEnvio = millis() + 1000;
        break;
      }
      xQueueReceive(filaEventos, &ev, 0);
    }

    if (millis() >= proximaConsulta) {
      proximaConsulta = millis() + CONSULTA_ALERTAS_MS;
      consultarAlertas();
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

void enviarEvento(const char *tipo) {
  Serial.printf("[EVENTO] tipo=%s\n", tipo);
  if (!MODO_API) return;
  EventoFila ev;
  strncpy(ev.tipo, tipo, sizeof(ev.tipo) - 1);
  ev.tipo[sizeof(ev.tipo) - 1] = '\0';
  ev.tMs = millis();
  if (xQueueSend(filaEventos, &ev, 0) != pdTRUE) {
    Serial.println("[REDE] fila de eventos cheia: evento descartado");
  }
}

// ---------- LED e buzzer ----------

void aplicarCor(Rgb c) {
  ledcWrite(PIN_R, c.r * BRILHO_MAX / 255);
  ledcWrite(PIN_G, c.g * BRILHO_MAX / 255);
  ledcWrite(PIN_B, c.b * BRILHO_MAX / 255);
}

void aplicarSom(int freq) {
  static int atual = 0;
  if (freq == atual) return;
  atual = freq;
  if (freq > 0) tone(PIN_BUZ, freq);
  else noTone(PIN_BUZ);
}

unsigned long duracaoMs(Alerta a) {
  switch (a) {
    case A_TURNO_ATIVO: return 150;
    case A_PONTO_CRITICO: return 900;
    case A_MENSAGEM: return 800;
    case A_CONTAGEM_QUEDA: return CONTAGEM_MS;
    case A_SOS_ENVIADO: return 0xFFFFFFFFUL;
    case A_CANCELADO: return 3000;
    case A_TURNO_ENCERRADO: return 600;
    case A_SEM_CONEXAO: return 800;
    default: return 0;
  }
}

uint8_t prioridade(Alerta a) {
  switch (a) {
    case A_CONTAGEM_QUEDA:
    case A_SOS_ENVIADO: return 6;
    case A_CANCELADO: return 5;
    case A_PONTO_CRITICO: return 4;
    case A_MENSAGEM: return 3;
    case A_TURNO_ENCERRADO: return 2;
    case A_TURNO_ATIVO: return 1;
    default: return 0;
  }
}

Rgb corDoEfeito(Alerta a, unsigned long t) {
  switch (a) {
    case A_TURNO_ATIVO: return t < 150 ? VERDE : DESLIGADO;
    case A_PONTO_CRITICO: return (t % 300 < 150) ? AMBAR : DESLIGADO;
    case A_MENSAGEM: return t < 800 ? AZUL : DESLIGADO;
    case A_CONTAGEM_QUEDA: return ((t / 125) % 2 == 0) ? VERMELHO : DESLIGADO;
    case A_SOS_ENVIADO:
      if (t < 10000) return VERMELHO;
      return ((t - 10000) % 2000 < 1000) ? VERMELHO : DESLIGADO;
    case A_CANCELADO: return VERDE;
    case A_TURNO_ENCERRADO: return (t % 300 < 150) ? VERDE : DESLIGADO;
    case A_SEM_CONEXAO: return BRANCO;
    default: return DESLIGADO;
  }
}

int somDoEfeito(Alerta a, unsigned long t) {
  switch (a) {
    case A_PONTO_CRITICO: return t < 100 ? SOM_BIPE_HZ : 0;
    case A_CONTAGEM_QUEDA:
      return (t % SOM_CONTAGEM_PERIODO_MS < SOM_CONTAGEM_BIPE_MS) ? SOM_CONTAGEM_HZ : 0;
    case A_SOS_ENVIADO: return (t < 100 || (t >= 200 && t < 300)) ? SOM_BIPE_HZ : 0;
    case A_CANCELADO: return t < 100 ? SOM_CANCELADO_HZ : 0;
    default: return 0;
  }
}

bool efeitoAtivo(unsigned long agora) {
  return efeito.tipo != A_NENHUM && (agora - efeito.inicio) < duracaoMs(efeito.tipo);
}

// Alerta de prioridade menor não interrompe um efeito em curso, a menos que seja forçado.
bool iniciarEfeito(Alerta a, bool forcar) {
  unsigned long agora = millis();
  if (!forcar && efeitoAtivo(agora) && prioridade(a) < prioridade(efeito.tipo)) return false;
  efeito.tipo = a;
  efeito.inicio = agora;
  return true;
}

void atualizarSaida(unsigned long agora) {
  Rgb cor = DESLIGADO;
  int freq = 0;
  if (efeitoAtivo(agora)) {
    unsigned long t = agora - efeito.inicio;
    cor = corDoEfeito(efeito.tipo, t);
    freq = somDoEfeito(efeito.tipo, t);
  } else if (efeito.tipo != A_NENHUM) {
    if (efeito.tipo == A_CONTAGEM_QUEDA) contagemExterna = false;
    efeito.tipo = A_NENHUM;
  }
  aplicarCor(cor);
  aplicarSom(freq);
}

void tratarPeriodicos(unsigned long agora) {
  if (efeitoAtivo(agora)) return;
  if (modoTurno && agora - tUltimoTurno >= TURNO_PULSO_A_CADA_MS) {
    tUltimoTurno = agora;
    iniciarEfeito(A_TURNO_ATIVO, false);
    return;
  }
  if (semConexao && agora - tUltimoSemConexao >= SEM_CONEXAO_A_CADA_MS) {
    tUltimoSemConexao = agora;
    iniciarEfeito(A_SEM_CONEXAO, false);
  }
}

// ---------- Máquina de estados da queda ----------

void mudarEstado(Estado novo) {
  if (novo == estado) return;
  Serial.printf("[ESTADO] %s -> %s\n", NOME_ESTADO[estado], NOME_ESTADO[novo]);
  estado = novo;
}

void iniciarContagem(unsigned long agora) {
  tContagem = agora;
  mudarEstado(CONTAGEM);
  enviarEvento("impacto");
  iniciarEfeito(A_CONTAGEM_QUEDA, true);
}

void irParaSos() {
  contagemExterna = false;
  mudarEstado(SOS_ENVIADO);
  iniciarEfeito(A_SOS_ENVIADO, true);
}

void cancelarTudo(bool avisar) {
  contagemExterna = false;
  mudarEstado(MONITORANDO);
  if (avisar) iniciarEfeito(A_CANCELADO, true);
}

// Contagem esgotada sem cancelar: o vestível pede o SOS.
void tratarContagem(unsigned long agora) {
  if (estado == CONTAGEM && agora - tContagem >= CONTAGEM_MS) {
    enviarEvento("sos");
    irParaSos();
  }
}

// ---------- MPU6050 ----------

const uint8_t MPU_ADDR = 0x68;

bool escreverMpu(uint8_t reg, uint8_t valor) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(reg);
  Wire.write(valor);
  return Wire.endTransmission() == 0;
}

bool iniciarMpu() {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x75);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)MPU_ADDR, 1) != 1) return false;
  Wire.read();
  bool ok = escreverMpu(0x6B, 0x00);
  ok &= escreverMpu(0x1C, 0x10);  // +-8 g: o padrão de +-2 g não alcança o limiar de impacto
  ok &= escreverMpu(0x1B, 0x08);  // +-500 graus/s
  return ok;
}

bool lerMpu(float &ax, float &ay, float &az, float &gx, float &gy, float &gz) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x3B);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)MPU_ADDR, 14) != 14) return false;
  int16_t v[7];
  for (int i = 0; i < 7; i++) {
    int alto = Wire.read();
    int baixo = Wire.read();
    v[i] = (int16_t)((alto << 8) | baixo);
  }
  ax = v[0] / 4096.0f;
  ay = v[1] / 4096.0f;
  az = v[2] / 4096.0f;
  gx = v[4] / 65.5f;
  gy = v[5] / 65.5f;
  gz = v[6] / 65.5f;
  return true;
}

void tratarImu(unsigned long agora) {
  static unsigned long ultimaLeitura = 0;
  if (agora - ultimaLeitura < IMU_PERIODO_MS) return;
  ultimaLeitura = agora;

  float ax, ay, az, gx, gy, gz;
  if (!lerMpu(ax, ay, az, gx, gy, gz)) return;
  float modulo = sqrtf(ax * ax + ay * ay + az * az);
  float giro = sqrtf(gx * gx + gy * gy + gz * gz);
  bool parado = fabsf(modulo - 1.0f) < LIMIAR_PARADO_G && giro < LIMIAR_GIRO_DPS;

#if MODO_TESTE
  static unsigned long ultimoDebug = 0;
  if (debugImu && agora - ultimoDebug >= 500) {
    ultimoDebug = agora;
    Serial.printf("[IMU] |a|=%.2f g  giro=%.1f graus/s  parado=%d\n", modulo, giro, parado);
  }
#endif

  switch (estado) {
    case MONITORANDO:
      // Com contagem vinda da API o vestível só exibe, para não duplicar.
      if (!contagemExterna && modulo > LIMIAR_IMPACTO_G) {
        Serial.printf("[IMU] pico de %.2f g\n", modulo);
        tEstado = agora;
        mudarEstado(IMPACTO_CANDIDATO);
      }
      break;

    case IMPACTO_CANDIDATO:
      if (agora - tEstado >= ASSENTAR_MS) {
        tEstado = agora;
        tParadoDesde = 0;
        mudarEstado(VERIFICANDO_IMOBILIDADE);
      }
      break;

    case VERIFICANDO_IMOBILIDADE:
      if (parado) {
        if (tParadoDesde == 0) tParadoDesde = agora;
        if (agora - tParadoDesde >= IMOBILIDADE_MS) {
          iniciarContagem(agora);
          break;
        }
      } else {
        tParadoDesde = 0;
      }
      if (agora - tEstado >= JANELA_VERIF_MS) {
        Serial.println("[IMU] sem imobilidade na janela: falso alarme");
        mudarEstado(MONITORANDO);
      }
      break;

    default:
      break;
  }
}

// ---------- Botão ----------

void toqueCurto() {
  if (estado == CONTAGEM || contagemExterna) {
    enviarEvento("cancelar");
    cancelarTudo(true);
  } else if (estado == IMPACTO_CANDIDATO || estado == VERIFICANDO_IMOBILIDADE) {
    Serial.println("[BOTAO] queda candidata descartada");
    mudarEstado(MONITORANDO);
  }
}

void segurarSos() {
  if (estado == SOS_ENVIADO) return;
  Serial.println("[BOTAO] segurado: SOS");
  enviarEvento("sos");
  irParaSos();
}

void segurarCancelarSos() {
  if (estado != SOS_ENVIADO) return;
  Serial.println("[BOTAO] segurado: SOS cancelado");
  enviarEvento("cancelar");
  cancelarTudo(true);
}

void tratarBotao(unsigned long agora) {
  static bool anterior = false;
  static bool longoDisparado = false;
  static bool cancelaSos = false;  // o SOS já estava ativo quando o botão foi pressionado
  static unsigned long tPressionado = 0;
  static unsigned long tMudanca = 0;

  bool pressionado = digitalRead(PIN_BTN) == LOW;
  if (pressionado != anterior && agora - tMudanca >= DEBOUNCE_MS) {
    tMudanca = agora;
    anterior = pressionado;
    if (pressionado) {
      tPressionado = agora;
      longoDisparado = false;
      cancelaSos = estado == SOS_ENVIADO;
    } else if (!longoDisparado) {
      toqueCurto();
    }
  }
  if (!anterior || longoDisparado) return;

  unsigned long segurado = agora - tPressionado;
  if (cancelaSos && segurado >= CANCELAR_SOS_SEGURAR_MS) {
    longoDisparado = true;
    segurarCancelarSos();
  } else if (!cancelaSos && segurado >= SOS_SEGURAR_MS) {
    longoDisparado = true;
    segurarSos();
  }
}

// ---------- Alertas ----------

void aplicarAlerta(Alerta a) {
  unsigned long agora = millis();
  Serial.printf("[ALERTA] %s\n", NOME_ALERTA[a]);

  switch (a) {
    case A_CONTAGEM_QUEDA:
      if (estado != MONITORANDO) return;  // já contando: não reinicia
      if (iniciarEfeito(a, false)) contagemExterna = true;
      break;

    case A_SOS_ENVIADO:
      irParaSos();  // o app já registrou o SOS; não reenvia
      break;

    case A_CANCELADO:
      cancelarTudo(false);
      iniciarEfeito(a, true);
      break;

    case A_TURNO_ENCERRADO:
      modoTurno = false;
      if (estado == CONTAGEM || contagemExterna) cancelarTudo(false);
      iniciarEfeito(a, estado != SOS_ENVIADO);
      break;

    case A_TURNO_ATIVO:
      modoTurno = true;
      tUltimoTurno = agora;
      iniciarEfeito(a, false);
      break;

    default:
      iniciarEfeito(a, false);
      break;
  }
}

void tratarFilaAlertas() {
  if (!filaAlertas) return;
  Alerta a;
  while (xQueueReceive(filaAlertas, &a, 0) == pdTRUE) aplicarAlerta(a);
}

// ---------- Comandos de teste (Serial) ----------
#if MODO_TESTE

void mostrarAjuda() {
  Serial.println("Teclas (alertas que o app enviaria):");
  Serial.println("  1 turno_ativo   2 ponto_critico   3 mensagem   4 contagem_queda");
  Serial.println("  5 sos_enviado   6 cancelado       7 turno_encerrado");
  Serial.println("  8 liga/desliga 'sem conexao'      d liga/desliga leitura do sensor");
}

void tratarSerial(unsigned long agora) {
  while (Serial.available()) {
    char c = Serial.read();
    switch (c) {
      case '1': aplicarAlerta(A_TURNO_ATIVO); break;
      case '2': aplicarAlerta(A_PONTO_CRITICO); break;
      case '3': aplicarAlerta(A_MENSAGEM); break;
      case '4': aplicarAlerta(A_CONTAGEM_QUEDA); break;
      case '5': aplicarAlerta(A_SOS_ENVIADO); break;
      case '6': aplicarAlerta(A_CANCELADO); break;
      case '7': aplicarAlerta(A_TURNO_ENCERRADO); break;
      case '8':
        if (MODO_API) {
          Serial.println("[REDE] no modo API, o estado da conexao vem da rede");
          break;
        }
        semConexao = !semConexao;
        tUltimoSemConexao = agora - SEM_CONEXAO_A_CADA_MS;
        Serial.printf("[REDE] sem conexao = %s\n", semConexao ? "sim" : "nao");
        break;
      case 'd':
        debugImu = !debugImu;
        Serial.printf("[IMU] leitura na Serial = %s\n", debugImu ? "ligada" : "desligada");
        break;
      case '\r':
      case '\n':
      case ' ': break;
      default: mostrarAjuda(); break;
    }
  }
}

#endif

// ---------- Arduino ----------

void setup() {
  Serial.begin(115200);
  pinMode(PIN_BTN, INPUT_PULLUP);
  ledcAttach(PIN_R, 5000, 8);
  ledcAttach(PIN_G, 5000, 8);
  ledcAttach(PIN_B, 5000, 8);
  aplicarCor(DESLIGADO);

  Wire.begin(PIN_SDA, PIN_SCL);
  if (!iniciarMpu()) Serial.println("MPU6050 NAO respondeu: confira SDA, SCL e AD0");

  Serial.printf("Pulseira Garupa SP (%s) dispositivoId=%s\n", MODO_API ? "modo API" : "modo simulado", DISPOSITIVO_ID);
#if MODO_TESTE
  mostrarAjuda();
#endif

  if (MODO_API) {
    filaEventos = xQueueCreate(FILA_EVENTOS, sizeof(EventoFila));
    filaAlertas = xQueueCreate(8, sizeof(Alerta));
    // Prioridade 0: divide o núcleo 0 com a tarefa ociosa, que alimenta o watchdog durante o handshake TLS.
    xTaskCreatePinnedToCore(tarefaRede, "rede", 16384, nullptr, tskIDLE_PRIORITY, nullptr, 0);
  }
}

void loop() {
  unsigned long agora = millis();
#if MODO_TESTE
  tratarSerial(agora);
#endif
  tratarFilaAlertas();
  tratarBotao(agora);
  tratarImu(agora);
  tratarContagem(agora);
  tratarPeriodicos(agora);
  atualizarSaida(agora);
  delay(1);
}
