#pragma once

// --- Teste ---
#define MODO_TESTE 0  // 1 habilita as teclas da Serial (alertas simulados e leitura do sensor)

// --- API ---
#define API_BASE_URL "https://garupa-sp-api-production.up.railway.app/api/v1"
#define API_TELEFONE "11912345678"  // usuário de teste fictício a quem a pulseira está pareada
#define API_SENHA "wearable_test001"
#define DISPOSITIVO_ID "pulseira-garupa-002"
#define WIFI_SSID "Wokwi-GUEST"
#define WIFI_CANAL 6  // informar o canal poupa cerca de 4 s na conexão
#define NTP_SERVIDOR_1 "pool.ntp.org"
#define NTP_SERVIDOR_2 "time.google.com"
#define CONSULTA_ALERTAS_MS 2000UL
#define HTTP_TIMEOUT_MS 2500UL    // resposta, com a conexão já aberta
#define HTTP_CONEXAO_MS 10000UL   // abrir a conexão (handshake TLS)
#define FALHAS_PARA_SEM_CONEXAO 3
#define ESPERA_LOGIN_RECUSADO_MS 30000UL
#define ESPERA_LIMITE_LOGIN_MS 60000UL
#define ESPERA_LOGIN_FALHO_MS 5000UL
#define FILA_EVENTOS 8
#define MARGEM_RELOGIO_S 1  // ocorridoEm = agora - margem, pois a API recusa horário futuro

// --- Pinos ---
#define PIN_SDA 21
#define PIN_SCL 22
#define PIN_BTN 4
#define PIN_R 25
#define PIN_G 26
#define PIN_B 27
#define PIN_BUZ 18

// --- LED ---
#define BRILHO_MAX 90  // cerca de 35% de 255

// --- Buzzer ---
#define SOM_CONTAGEM_HZ 520
#define SOM_CONTAGEM_PERIODO_MS 500UL
#define SOM_CONTAGEM_BIPE_MS 80UL
#define SOM_BIPE_HZ 784
#define SOM_CANCELADO_HZ 660

// --- Detecção de queda ---
#define LIMIAR_IMPACTO_G 2.5f      // módulo da aceleração que abre o impacto candidato
#define LIMIAR_PARADO_G 0.15f      // distância máxima de 1 g para considerar parado
#define LIMIAR_GIRO_DPS 10.0f      // rotação máxima para considerar parado
#define ASSENTAR_MS 300UL          // espera após o pico, para o choque assentar
#define IMOBILIDADE_MS 2000UL      // tempo parado, sem interrupção, que confirma a queda
#define JANELA_VERIF_MS 5000UL     // prazo para confirmar a imobilidade
#define CONTAGEM_MS 15000UL
#define IMU_PERIODO_MS 20UL

// --- Botão ---
#define DEBOUNCE_MS 30UL
#define SOS_SEGURAR_MS 3000UL
#define CANCELAR_SOS_SEGURAR_MS 5000UL

// --- Luzes periódicas ---
#define TURNO_PULSO_A_CADA_MS 10000UL
#define SEM_CONEXAO_A_CADA_MS 3000UL
