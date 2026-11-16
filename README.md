# Garupa SP: pulseira (ESP32 no Wokwi)

Vestível opcional do **Garupa SP**, o app para motociclistas e entregadores por aplicativo da CitizenTech do grupo. A pulseira confirma que o corpo sofreu o impacto, deixa o motociclista cancelar ou pedir socorro sem pegar o celular e **repete no pulso, com luzes e sons, os alertas do app**.

- **Link do projeto no Wokwi:** [wokwi.com/projects/480690813997130753](https://wokwi.com/projects/480690813997130753)
- **Integrantes** (também em [integrantes.txt](integrantes.txt)):
  - Caio Alexandre dos Santos (RM 558460)
  - Leandro do Nascimento Souza (RM 558893)
  - Rafael de Mônaco Maniezo (RM 556079)
  - Vinicius Rozas Pannuci de Paula Cont (RM 555338)
- **Vídeo (até 4 min):** _(a preencher)_

## Como a pulseira entra na solução

O produto funciona sem ela: o app detecta a queda e pede socorro sozinho. A pulseira é uma segunda leitura (o celular no guidão sofre buraco e trepidação o tempo todo) e um botão que dispensa o celular.

```
Pulseira (ESP32) ── eventos: impacto, sos, cancelar ──▶ API Garupa SP ◀── app (consome os eventos e abre/cancela a contagem)
Pulseira (ESP32) ◀── alertas pendentes (consulta a cada 2 s) ── API ◀── app (envia turno_ativo, ponto_critico, ...)
```

- O Wokwi não simula Bluetooth no ESP32, então o circuito usa **WiFi** (`Wokwi-GUEST`) e **HTTPS** contra a API publicada. No produto real, o app falaria direto com a pulseira por Bluetooth, e a API ficaria só para os eventos.
- A pulseira **não envia posição**: ela vem do celular. Os payloads levam só `dispositivoId`, `tipo` e `ocorridoEm`.

## Componentes

| Peça | Função |
| --- | --- |
| ESP32 DevKit C V4 | Microcontrolador com WiFi |
| MPU6050 (I2C) | Único sensor: pico de aceleração do impacto e imobilidade depois dele |
| Botão | Cancelar a contagem, pedir SOS e cancelar o SOS |
| LED RGB (cátodo comum) + 3 resistores de 220 Ω | Luzes de alerta |
| Buzzer | Som dos alertas; também representa a vibração |

### Ligações

| Peça | Pino da peça | ESP32 |
| --- | --- | --- |
| MPU6050 | VCC / GND | 3V3 / GND |
| MPU6050 | SDA / SCL | GPIO21 / GPIO22 |
| MPU6050 | AD0 | GND (endereço `0x68`) |
| Botão | 1 / 2 | GPIO4 (`INPUT_PULLUP`) / GND |
| LED RGB | R / G / B | GPIO25 / GPIO26 / GPIO27, cada um por um resistor de 220 Ω |
| LED RGB | COM | GND |
| Buzzer | positivo / negativo | GPIO18 / GND |

O circuito completo está em [diagram.json](diagram.json).

## Como a pulseira se comporta

### Detecção de queda

`monitorando → impacto_candidato → verificando_imobilidade → contagem → sos_enviado`

1. **Pico:** o módulo da aceleração passa de **2,5 g** (`LIMIAR_IMPACTO_G`). O MPU6050 é configurado em ±8 g, porque o padrão de ±2 g nunca chegaria a esse valor.
2. **Imobilidade:** depois de 300 ms para o choque assentar, o corpo precisa ficar **parado por 2 s seguidos** dentro de uma janela de 5 s. "Parado" significa módulo a menos de 0,15 g de 1 g e rotação abaixo de 10 °/s. Se não ficar parado a tempo, é falso alarme e volta a `monitorando`.
3. **Contagem:** abre a contagem de **15 s** (`CONTAGEM_MS`) com LED vermelho piscando e bipes, e envia o evento `impacto`.
4. **SOS:** a contagem acaba sem cancelar, e a pulseira envia o evento `sos`.

### Botão

| Situação | Ação | Efeito |
| --- | --- | --- |
| Queda candidata em verificação | Toque curto | Descarta a queda, sem evento |
| Contagem em andamento (própria ou vinda do app) | Toque curto | Envia `cancelar`, volta a `monitorando`, luz verde |
| Qualquer estado sem SOS | Segurar **3 s** | Envia `sos` |
| SOS ativo | Toque curto | Nada acontece |
| SOS ativo | Segurar **5 s** | Envia `cancelar`, volta a `monitorando`, luz verde |

Para cancelar o SOS é preciso soltar e pressionar de novo: segurar 3 s para enviar o SOS e continuar segurando **não** o cancela aos 5 s.

### Luzes e sons dos alertas

Cada alerta tem cor **e** padrão de piscadas, para não depender só da cor (daltonismo, sol forte).

| Alerta (`tipo` da API) | Cor | Padrão | Som |
| --- | --- | --- | --- |
| `turno_ativo` | Verde | Pulso de 150 ms, repetido a cada 10 s até chegar `turno_encerrado` | Nenhum |
| `ponto_critico` | Âmbar | 3 piscadas rápidas, uma vez | Beep curto |
| `mensagem` | Azul | 1 piscada lenta (800 ms) | Nenhum |
| `contagem_queda` | Vermelho | Piscando a 4 Hz durante a contagem | Bipes espaçados (80 ms a cada 500 ms) |
| `sos_enviado` | Vermelho | Fixo por 10 s, depois piscada lenta | Dois bipes |
| `cancelado` | Verde | Fixo por 3 s | Um beep |
| `turno_encerrado` | Verde | Duas piscadas e apaga | Nenhum |
| Sem conexão com a API (estado da própria pulseira) | Branco | Uma piscada lenta a cada 3 s | Nenhum |

Quando dois alertas colidem, vale a prioridade: `contagem_queda` e `sos_enviado` > `cancelado` > `ponto_critico` > `mensagem` > `turno_encerrado` > `turno_ativo` > sem conexão. `cancelado` e `turno_encerrado` interrompem a contagem.

Quando o **app** abre a contagem (alerta `contagem_queda`) e a pulseira está ociosa, ela só **exibe** a contagem e **não** envia `impacto`, para não duplicar. Ao receber `sos_enviado`, ela mostra o SOS sem reenviá-lo.

## Comunicação com a API

Base: `https://garupa-sp-api-production.up.railway.app/api/v1`. A API redireciona HTTP para HTTPS, e o firmware usa HTTPS.

| Chamada | Quando |
| --- | --- |
| `POST /autenticacao/login` | Ao iniciar e sempre que a API responde 401. Corpo `{ telefone, senha }`, resposta `{ token, expiraEm }`. O token vale 1 semana, e o time da API o atualiza |
| `POST /eventos-vestivel` | Ao detectar `impacto`, `sos` ou `cancelar`. Corpo `{ dispositivoId, tipo, ocorridoEm }`; responde 202 |
| `GET /vestivel/{dispositivoId}/alertas/pendentes` | A cada 2 s. Devolve `[{ alertaId, tipo, ocorridoEm }]` e **consome** os alertas; cada um é entregue uma vez e expira em 30 s |

Regras da API que o firmware respeita:

- Todas as chamadas, menos o login, exigem `Authorization: Bearer <token>`.
- `ocorridoEm` vai em **UTC com `Z`** e **nunca no futuro**. Por isso o ESP32 sincroniza o relógio por NTP antes de enviar qualquer evento e usa `agora - 1 s`.
- O token precisa ser do **usuário pareado com o `dispositivoId`**. Dispositivo não pareado, ou pareado com outro usuário, recebe **403**.
- Eventos recusados com erro 4xx são descartados. Falhas de rede e erros 5xx ficam numa fila de até 8 eventos e são reenviados.
- Três consultas seguidas sem resposta acendem a luz branca.

## Configuração (`config.h`)

| Constante | O que controla |
| --- | --- |
| `API_BASE_URL` | Endereço da API. Vazia, a pulseira roda sem rede |
| `API_TELEFONE`, `API_SENHA` | Login do usuário de teste (dados fictícios) |
| `DISPOSITIVO_ID` | Identificador da pulseira; precisa estar pareado com esse usuário |
| `CONTAGEM_MS` | Duração da contagem (15 s) |
| `SOS_SEGURAR_MS`, `CANCELAR_SOS_SEGURAR_MS` | Tempos do botão (3 s e 5 s) |
| `LIMIAR_IMPACTO_G`, `LIMIAR_PARADO_G`, `LIMIAR_GIRO_DPS`, `IMOBILIDADE_MS`, `JANELA_VERIF_MS` | Calibração da detecção de queda |
| `BRILHO_MAX`, `SOM_*` | Brilho do LED e frequências do buzzer |
| `MODO_TESTE` | `1` habilita as teclas da Serial (veja o roteiro) |

## Passo a passo no Wokwi

### 1. Montar o projeto

1. Em [wokwi.com](https://wokwi.com), crie um projeto **ESP32** (Arduino).
2. Cole o conteúdo de `sketch.ino` na aba do sketch.
3. Crie uma aba `config.h` e cole o conteúdo de [config.h](config.h).
4. Substitua o conteúdo da aba `diagram.json` pelo de [diagram.json](diagram.json).
5. Não é preciso `libraries.txt`: o firmware usa só as bibliotecas padrão do ESP32 (`WiFi`, `HTTPClient`, `WiFiClientSecure`, `Wire`).

### 2. Usuário da API

O projeto já tem um **usuário de teste cadastrado** na API (dados fictícios): o telefone e a senha estão em `API_TELEFONE` e `API_SENHA` no `config.h`, e não é preciso criar outro. O `DISPOSITIVO_ID` do `config.h` precisa estar pareado com esse usuário; se a Serial mostrar `403`, veja "Problemas conhecidos".

Para os testes da seção D, que enviam alertas pelo Swagger (`https://garupa-sp-api-production.up.railway.app/swagger/index.html`):

1. Faça `POST /api/v1/autenticacao/login` com o telefone e a senha do `config.h`.
2. Clique em **Authorize** e cole o `token` da resposta, sem a palavra `Bearer`. Ele vale 1 semana.
3. Use o `usuarioId` desse usuário nas rotas `/usuarios/{usuarioId}/...`.

### 3. Rodar

Clique em **Start the simulation** e abra o monitor Serial. A sequência esperada na Serial é:

```
Pulseira Garupa SP (modo API) dispositivoId=...
[REDE] conectando ao WiFi...
[REDE] WiFi conectado
[REDE] autenticado
```

A conexão WiFi e o primeiro login podem levar alguns segundos: o handshake TLS é lento no simulador. Depois disso, a pulseira fica em `monitorando` e consulta alertas a cada 2 s.

## Roteiro de testes

Convenções:

- **MPU6050:** clique na peça no diagrama para abrir os controles de aceleração (X, Y, Z) e de rotação. O **repouso** é X = 0, Y = 0, Z = 1 g e rotação 0.
- **Alerta pelo Swagger:** `POST /api/v1/usuarios/{usuarioId}/alertas-vestivel` com `{"tipo":"<tipo>","ocorridoEm":"<agora em UTC, com Z>"}`, por exemplo `2026-11-16T22:30:00Z`. O horário não pode estar no futuro. Use um horário de poucos segundos atrás, porque o alerta expira em 30 s.
- **Eventos na API:** `GET /api/v1/eventos-vestivel/pendentes?usuarioId={usuarioId}`. Essa consulta **consome** os eventos: cada um sai uma vez.

### A. Inicialização

| # | Passo | Resultado esperado |
| --- | --- | --- |
| A1 | Iniciar a simulação | Serial mostra `WiFi conectado` e `autenticado`. Não aparece `MPU6050 NAO respondeu` nem `sem conexao` |
| A2 | Observar o LED por 10 s | Apagado, sem reinício do ESP32 (se reiniciar, veja a seção de problemas) |

### B. Detecção de queda

| # | Passo | Resultado esperado |
| --- | --- | --- |
| B1 | Levar a aceleração X a **3 g** por um instante e voltar ao repouso (0, 0, 1). Não mexer em nada por 2 s | Serial: `pico de ... g`, depois `impacto_candidato -> verificando_imobilidade -> contagem`. LED vermelho a 4 Hz com bipes. Serial: `[EVENTO] tipo=impacto` e, em seguida, `evento impacto registrado (202)` |
| B2 | Deixar a contagem correr os 15 s | Serial: `[EVENTO] tipo=sos`, estado `sos_enviado`. LED vermelho fixo e dois bipes; depois de 10 s, piscada lenta. A API registra o `sos` |
| B3 | Levar X a 3 g e **deixar lá** | Em 5 s, Serial: `sem imobilidade na janela: falso alarme`, e volta a `monitorando`. Nenhum evento |
| B4 | Repetir B1 e, durante a contagem, fazer um **toque curto** | Serial: `[EVENTO] tipo=cancelar` e `evento cancelar registrado (202)`. LED verde fixo por 3 s com um beep, e volta a `monitorando` |
| B5 | Levar X a 3 g e, na verificação, fazer um toque curto | Serial: `queda candidata descartada`. Nenhum evento |

### C. Botão e SOS manual

| # | Passo | Resultado esperado |
| --- | --- | --- |
| C1 | Em `monitorando`, segurar o botão **3 s** e soltar | Serial: `segurado: SOS` e `[EVENTO] tipo=sos`. LED vermelho fixo, dois bipes |
| C2 | Com o SOS ativo, fazer um **toque curto** | Nada acontece. O SOS continua |
| C3 | Com o SOS ativo, **segurar 5 s** | Serial: `segurado: SOS cancelado` e `[EVENTO] tipo=cancelar`. LED verde por 3 s e um beep, e volta a `monitorando` |
| C4 | Em `monitorando`, segurar o botão por **6 s** sem soltar | Aos 3 s envia o `sos`. Aos 5 s **não** cancela. Só cancela depois de soltar e segurar 5 s de novo |

### D. Alertas vindos do app (use o Swagger)

Envie cada alerta e confira a luz e o som. Meça o tempo entre o envio e a luz: deve ser **de até 3 s**.

| # | `tipo` | Resultado esperado |
| --- | --- | --- |
| D1 | `ponto_critico` | Âmbar, 3 piscadas, beep curto |
| D2 | `mensagem` | Azul, 1 piscada lenta, sem som |
| D3 | `turno_ativo` | Pulso verde agora e a cada 10 s, sem som |
| D4 | `turno_encerrado` | Duas piscadas verdes e apaga; o pulso de 10 s para |
| D5 | `contagem_queda` | Vermelho a 4 Hz com bipes por 15 s. **Não** aparece `[EVENTO] tipo=impacto` na Serial |
| D6 | `contagem_queda` e, durante ela, um toque curto | Envia `cancelar`, LED verde por 3 s |
| D7 | `contagem_queda` e, durante ela, `cancelado` | A contagem para; verde fixo por 3 s |
| D8 | `sos_enviado` | Vermelho fixo e dois bipes. **Não** reenvia `sos` |
| D9 | `cancelado` com o SOS ativo | Volta a `monitorando`; verde fixo por 3 s |
| D10 | Enviar o mesmo alerta e esperar | Acende **uma vez só**: cada alerta é entregue uma vez |

### E. Falhas

| # | Passo | Resultado esperado |
| --- | --- | --- |
| E1 | Trocar `DISPOSITIVO_ID` por um id **não pareado** e reiniciar | Serial: `sem permissao (403): o login e o pareamento devem ser do mesmo usuario`, uma única vez. LED **sem** luz branca |
| E2 | Colocar uma senha errada em `API_SENHA` e reiniciar | Serial: `login recusado (401)`, com nova tentativa a cada 30 s. Nenhum alerta é consultado |
| E3 | Trocar `API_BASE_URL` por `https://invalido.example/api/v1` e reiniciar | Depois de 3 falhas, Serial: `sem conexao com a API`; LED branco com uma piscada lenta a cada 3 s |
| E4 | Voltar `API_BASE_URL` ao valor correto e reiniciar | Serial: `autenticado`; sem luz branca |
| E5 | Provocar uma queda **antes** de a pulseira autenticar (logo ao iniciar) | O evento fica na fila e é enviado quando houver login e relógio sincronizado |

### F. Opcional: comandos de teste pela Serial

Com `MODO_TESTE` em `1` no `config.h`, a Serial aceita teclas que injetam alertas sem passar pela API. Só para desenvolvimento: **volte a `0` antes de gravar o vídeo e de entregar**.

| Tecla | Efeito |
| --- | --- |
| `1` a `7` | Injeta `turno_ativo`, `ponto_critico`, `mensagem`, `contagem_queda`, `sos_enviado`, `cancelado`, `turno_encerrado` |
| `8` | Liga e desliga "sem conexão" (só com `API_BASE_URL` vazia) |
| `d` | Mostra o módulo da aceleração e da rotação a cada 500 ms. Em repouso, `\|a\|` deve ficar perto de 1,00 g |

## Problemas conhecidos

| Sintoma | Causa e solução |
| --- | --- |
| O ESP32 reinicia sozinho depois de `WiFi conectado`, com `task_wdt` na Serial | O handshake TLS ocupava o núcleo 0 inteiro. O firmware já cria a tarefa de rede com prioridade 0 para evitar isso; se voltar, confirme que o sketch é o atual |
| `403` logo depois de `autenticado` | O `DISPOSITIVO_ID` não está pareado com o usuário do login. Pareie de novo ou use um id novo |
| `401` a cada 30 s | `API_TELEFONE` ou `API_SENHA` errados |
| `429` no login | Limite de tentativas da API; o firmware espera 60 s |
| `MPU6050 NAO respondeu` | Confira SDA (GPIO21), SCL (GPIO22) e AD0 no GND |
| Alerta enviado não acende a luz | O horário `ocorridoEm` estava no futuro ou o alerta passou de 30 s. Outra consulta ao mesmo endpoint também pode tê-lo consumido |

## Limites desta simulação

- **Bluetooth:** o Wokwi não o simula no ESP32; a pulseira usa WiFi e a API como ponte.
- **Credenciais no firmware:** `API_TELEFONE` e `API_SENHA` existem só para a simulação. No produto real, o app entregaria o token à pulseira durante o pareamento, e ela nunca guardaria a senha.
- **Certificado TLS:** o firmware usa `setInsecure()`. Em produção, validaria o certificado da API.
- **Buzzer:** o simulador toca uma só frequência por vez. Por isso a contagem usa **bipes espaçados** em vez de tom contínuo.
- **Contagem de 15 s:** o contrato geral prevê 30 s (15 s só quando celular e vestível detectam juntos). A pulseira usa 15 s, configurável em `CONTAGEM_MS`.
- **Visibilidade:** o brilho do LED é limitado a cerca de 35%. Se a luz é visível com sol, luva e manga só se testa com a peça física.

## Estrutura da pasta

```
garupa-sp-wearable/
├── README.md             este arquivo
├── diagram.json          circuito do Wokwi
├── sketch.ino            firmware
├── config.h              configuração (pinos, limiares, API)
└── integrantes.txt       nomes dos integrantes
```
