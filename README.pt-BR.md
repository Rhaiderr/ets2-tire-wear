# ETS2 & ATS Tire Wear

[English](README.md) | **Português (Brasil)**

[![Baixar ets2-tire-wear.zip](https://img.shields.io/github/v/release/Rhaiderr/ets2-tire-wear?label=Baixar%20ets2-tire-wear.zip&style=for-the-badge&logo=github&color=2ea44f)](https://github.com/Rhaiderr/ets2-tire-wear/releases/latest/download/ets2-tire-wear.zip)
[![Buy Me a Coffee](https://img.shields.io/badge/Buy%20Me%20a%20Coffee-apoie-yellow?style=for-the-badge&logo=buymeacoffee&logoColor=white)](https://buymeacoffee.com/leonardoarouck)
[![PIX](https://img.shields.io/badge/PIX-Apoiar%20via%20Pix-32BCAD?style=for-the-badge&logo=pix&logoColor=white)](#apoie-meu-trabalho)

**[⬇ Baixar a versão mais recente (zip)](https://github.com/Rhaiderr/ets2-tire-wear/releases/latest/download/ets2-tire-wear.zip)** · [todas as versões](https://github.com/Rhaiderr/ets2-tire-wear/releases) · **Pix:** `fe1a4c11-6bb5-4844-b687-1f9143512862`

**Instalar:** copie os **dois arquivos** do zip, `ets2_tire_wear.dll` e `ets2_tire_wear.ini`, para `<pasta do jogo>\bin\win_x64\plugins\` (ETS2 ou ATS) e abra o jogo ([detalhes](#instalação)).

> **Versão beta.** Testada no ETS2 1.61 e no ATS 1.61, no teclado e num volante Logitech G923. Use em single player e faça backup do seu perfil se quiser segurança extra.

> **Os pneus podem estourar.** A partir de 60% de desgaste, um pneu pode estourar enquanto você dirige: o caminhão puxa para o lado e fica limitado a 60 km/h até você trocar os pneus. Isso vem ligado. Para desligar, use `[blowout] enabled = 0` no INI ([detalhes](#estouro-de-pneu)).

> **Pneus de mods ainda não são compatíveis.** Só os pneus do jogo base e das DLCs oficiais (Goodyear, Michelin) ganham a aparência gasta. Se você colocar um pneu de mod, o plugin não o reconhece e o desgaste visual simplesmente não acontece: esse pneu fica com a aparência original. Nada quebra, e os outros pneus continuam funcionando.

Plugin para **Euro Truck Simulator 2** e **American Truck Simulator** que mostra os pneus do **seu** caminhão e das **suas** carretas gastos de acordo com o **desgaste real** do jogo. O jogo já calcula o desgaste de cada roda (você vê no menu de manutenção), mas o pneu continua visualmente novo. Com o plugin, a banda de rodagem vai perdendo o desenho até ficar quase lisa.

![Os 8 níveis de desgaste do pneu padrão do caminhão](docs/images/niveis.png)

*Pneu padrão do caminhão do ETS2 nos 8 níveis de desgaste. Imagens geradas pela ferramenta de prévia do projeto, que usa exatamente o mesmo código do plugin, com uma luz simulada.*

### No jogo

<p align="center">
  <img src="docs/images/jogo_ets2_gasto.jpg" alt="Pneus traseiros gastos de um caminhão no ETS2" width="49%">
  <img src="docs/images/jogo_ats_gasto.jpg" alt="Pneus gastos de um Western Star no ATS" width="49%">
</p>

*Prints do jogo: pneus traseiros gastos de um caminhão no ETS2 (esquerda) e de um Western Star no ATS (direita).*

## O que ele faz

- **Lê o desgaste pela telemetria oficial** (SCS Telemetry SDK): um valor por caminhão e por carreta, de 0% a 100%.
- **Escolhe um de 8 níveis** (0, 14, 29, 43, 57, 71, 86 e 100%). O nível 0 é o pneu original do jogo.
- **Gasta só a banda de rodagem**: a lateral do pneu, as letras e o aro ficam intactos.
- **Só nos seus veículos**: os pneus do tráfego continuam novos, e isso vale também nos retrovisores.
- **Sem arquivos do jogo modificados**: nenhum `.scs` é alterado. As texturas gastas são criadas na memória, a partir das texturas originais, enquanto o jogo roda.
- **Estouro de pneu**: pneus muito gastos podem estourar, com aviso, som, pneu rasgado e efeito na direção ([detalhes](#estouro-de-pneu)).

### Como o desgaste aparece

| Desgaste | O que muda |
|---|---|
| Baixo | Os sulcos começam a ficar rasos e mais claros, e a linha colorida de "pneu novo" (alguns Michelin) some |
| Médio | Poeira acinzentada nos sulcos, os ombros se desgastam menos que o centro e o brilho fica mais fosco |
| Alto | Superfície quase lisa, cor de carvão, com grão de abrasão no sentido do giro, pequenas lascas, ondulação e indicadores de desgaste (TWI) aparecendo nos sulcos |

![Seis modelos a 0, 57 e 100%](docs/images/modelos.png)

![Detalhe por pixel](docs/images/detalhes.png)

### American Truck Simulator

O ATS tem pneus próprios (medidas e desenhos americanos), e eles recebem o mesmo tratamento.

![Seis modelos de pneu do ATS a 0, 57 e 100%](docs/images/ats_modelos.png)

## Estouro de pneu

O jogo não tem estouro de pneu, então o plugin cria um. A partir de 60% de desgaste, cada veículo (o caminhão e cada carreta) tem uma chance de estourar um pneu a cada quilômetro. Essa chance cresce com o desgaste:

| Desgaste | Distância média até estourar |
|---|---|
| até 60% | nunca estoura |
| 70% | ~19.000 km |
| 80% | ~2.400 km |
| 90% | ~700 km |
| 100% | ~300 km |

### Quando um pneu estoura

- **Aviso** no topo da tela, no estilo dos avisos do jogo, dizendo qual pneu estourou (veículo, eixo e lado). O texto segue o idioma do jogo: português quando o jogo está em português, inglês nos outros idiomas.

  ![Aviso de pneu estourado em português e em inglês](docs/images/aviso_estouro.png)

- **Som** do estouro e, depois, do pneu rasgado batendo no asfalto no ritmo da roda.
- **Pneu rasgado** só na roda que estourou: a malha de aço aparece na banda de rodagem e a lateral ganha um rasgo de tamanho sorteado. Tudo isso por cima do desgaste que o pneu já tinha.

  ![Pneu dianteiro esquerdo estourado no jogo, sobre um pneu já gasto](docs/images/pneu_estourado_jogo.png)

  *No jogo: pneu dianteiro esquerdo estourado num Scania, por cima de um desgaste de 86%.*

  ![Banda nova, estourada a 0% e estourada a 57% de desgaste, e o rasgo na lateral](docs/images/pneu_estourado.png)

  *Pneu Goodyear KMAX D: banda nova, estourada a 0% de desgaste, estourada a 57% e a lateral com o rasgo. Imagens da ferramenta de prévia do projeto.*

- **Efeito no caminhão:**
  - **Pneu do caminhão:** dá um tranco para o lado do pneu estourado e, depois, o caminhão continua puxando para esse lado, com uma vibração leve no volante. Ele também fica mais pesado.
  - **Pneu da carreta:** o caminhão só fica mais pesado, sem puxar o volante.
  - **Mais pesado** quer dizer menos acelerador e, com carreta engatada, o freio da carreta a 35%, que **não acende** a luz de freio. Só no momento do estouro há um toque curto no freio, e aí a luz pisca.
  - **Limite de velocidade:** 60 km/h com pneu do caminhão estourado e 70 km/h quando só a carreta tem pneu estourado. Com pedal analógico, o acelerador é cortado aos poucos perto do limite. Se você passar 8 km/h do limite e a velocidade não estiver caindo (numa descida, por exemplo), entra o freio, e a luz acende. Logo depois de um estouro em alta velocidade, o freio não entra: o corte do acelerador já está reduzindo a velocidade.

### Conserto e desligamento

- **Conserto:** troque os pneus na oficina. O estouro acaba quando o desgaste daquele veículo cai (pelo menos 5 pontos, ou metade do desgaste que ele tinha no estouro, o que for menor). Até lá, o estouro continua mesmo se você sair e voltar ao jogo.
- **Multiplayer:** no Convoy e no TruckersMP os pneus não estouram.
- **Desligar:**
  - `[blowout] enabled = 0` desliga os estouros;
  - `[effect] enabled = 0` mantém os estouros (aviso e pneu rasgado), mas tira o efeito no caminhão e o som;
  - **Ctrl+Shift+B** no jogo para o efeito e a batida do pneu até o fim da sessão.

O acelerador e os freios são somados aos seus pedais ou ao teclado pelos controles oficiais do SDK da SCS. Nenhuma memória do jogo é alterada. No **teclado**, o jogo não deixa o plugin reduzir o acelerador, então o limite de velocidade só age pelo freio, a partir de 8 km/h acima do limite.

### Volante com force feedback

Com um volante com force feedback, a puxada é uma **força no próprio volante**: ele gira para o lado do pneu estourado e você segura, como num estouro de verdade. O plugin acha sozinho o volante que o jogo usa (DirectInput) e soma essa força ao force feedback do jogo. No teclado ou num controle sem force feedback, a puxada vira um desvio na direção.

> **Testado só no Logitech G923.** Outros volantes que o jogo usa pelo DirectInput (Logitech, Thrustmaster, Fanatec, Moza…) devem funcionar do mesmo jeito, mas ainda não foram testados. Se testar com outro volante, abra uma [issue](../../issues) com o `game.log.txt` (`Documentos\<jogo>\game.log.txt`): as linhas `ffb:` mostram qual dispositivo foi escolhido.

- **Ajustar a força:** `ffb_pull` (puxada constante, padrão `0.25`), `ffb_kick` (tranco no estouro, `0.6`) e `ffb_wobble` (vibração, `0.08`), em frações da força máxima do volante. A mesma fração pesa bem mais num volante direct drive do que num G923: comece com metade dos valores e suba aos poucos. O ganho do software do volante (G HUB, Fanalab…) também conta.
- **Força para o lado errado:** `ffb_invert = 1`.
- **O plugin escolheu outro dispositivo** (por exemplo um joystick virtual) ou não achou o volante: escreva parte do nome do volante em `ffb_device` (por exemplo `ffb_device = G923`). O nome aparece no `game.log.txt`, nas linhas `ffb:`.
- **Sem força no volante:** `ffb = 0` volta ao desvio na direção.

## Compatibilidade

| Item | Situação |
|---|---|
| Jogos | ETS2 **1.61** e ATS **1.61** (únicas versões testadas). A mesma DLL serve nos dois: ela vê qual jogo a carregou |
| Sistema | Windows 10/11 64 bits |
| Renderização | **DirectX 11**; outros modos não foram testados |
| Pneus (ETS2) | Os 33 pares de textura do jogo base e das DLCs de pneus **Goodyear** e **Michelin** |
| Pneus (ATS) | Os 42 pares de textura de caminhão e carreta do jogo base, das DLCs de pneus **Goodyear** e **Michelin** e o pneu Duty da DLC de rodas (`dlc_rims`) |
| Pneus de mods | **Ainda não são compatíveis**: sem desgaste visual, o pneu fica com a aparência original |
| Caminhões e carretas de mods | Devem funcionar, porque a separação jogador × tráfego usa a posição das rodas e não o modelo |
| TruckersMP | **Não testado.** Os estouros se desligam sozinhos no TruckersMP e no Convoy |
| Controles (estouro) | Testado no **teclado** e no **Logitech G923** (força no volante). Outros volantes: [veja aqui](#volante-com-force-feedback) |
| Outros mods gráficos | Testado junto com um mod de iluminação que também usa `dxgi.dll` / `dinput8.dll` sem conflito |
| Desempenho | Sem perda de FPS nos testes. Cada textura de pneu gera 7 versões gastas numa thread separada: ~0,15 s para 512 px, ~0,7 s para 1024 px, só no carregamento. O pneu rasgado é feito uma vez por estouro, também numa thread separada (~75 ms para 1024 px) |

## Instalação

1. Baixe o **[ets2-tire-wear.zip](https://github.com/Rhaiderr/ets2-tire-wear/releases/latest/download/ets2-tire-wear.zip)** (versão mais recente; as anteriores ficam em [Releases](https://github.com/Rhaiderr/ets2-tire-wear/releases)).
2. Com o jogo **fechado**, copie os dois arquivos do zip, `ets2_tire_wear.dll` e `ets2_tire_wear.ini`, para `<pasta do jogo>\bin\win_x64\plugins\`. Crie a pasta `plugins` se ela não existir.
   - ETS2 (Steam): `C:\Program Files (x86)\Steam\steamapps\common\Euro Truck Simulator 2\bin\win_x64\plugins\`
   - ATS (Steam): `C:\Program Files (x86)\Steam\steamapps\common\American Truck Simulator\bin\win_x64\plugins\`
   - Joga os dois? Copie os mesmos dois arquivos para a pasta `plugins` de cada jogo.
3. Abra o jogo. Na primeira vez aparece o aviso de que um plugin usa recursos avançados do SDK: confirme com **OK**.

Só isso: os catálogos de pneus dos dois jogos vêm dentro da DLL, e a configuração é o `ets2_tire_wear.ini` ao lado dela. Os logs e o estado dos estouros ficam numa pasta `TireWear`, criada junto dos perfis do jogo: `Documentos\Euro Truck Simulator 2\TireWear\` ou `Documentos\American Truck Simulator\TireWear\`. Daqui em diante, `Documentos\<jogo>\TireWear` é a pasta do jogo que você usa.

O desgaste aparece conforme o pneu desgasta no seu save. Num pneu novo não muda nada. Para ver o efeito na hora, use `wear_override` (próxima seção).

**Desinstalar:** apague `ets2_tire_wear.dll` e `ets2_tire_wear.ini` da pasta `plugins` e a pasta `Documentos\<jogo>\TireWear`.

## Configuração

O arquivo é o `ets2_tire_wear.ini` da pasta `plugins` (vem no zip, um por jogo). Sem ele, o plugin cria e usa `Documentos\<jogo>\TireWear\ets2_tire_wear.ini`, como nas versões anteriores; o `game.log.txt` mostra qual está em uso (linha `settings:`). Cada chave tem um comentário explicando o que faz. Edite com o jogo fechado.

| Seção | Chave | Padrão | Para que serve |
|---|---|---|---|
| `[tires]` | `wear_levels` | `1` | Liga os pneus gastos (`0` desliga) |
| `[tires]` | `separate` | `1` | Só os seus veículos gastam (`0` gasta também o tráfego, pela regra da pasta da textura) |
| `[tires]` | `levels` | `8` | Quantidade de níveis de desgaste |
| `[tires]` | `wear_override` | `-1` | Teste: `0.0` a `1.0` força esse desgaste em todos os pneus; `-1` usa a telemetria |
| `[present]` | `notice` | `1` | Aviso no topo da tela quando um pneu estoura (`0` desliga) |
| `[present]` | `notice_s`, `notice_top`, `notice_scale` | `8`, `60`, `1` | Tempo do aviso na tela (segundos), distância do topo (em 1/900 da altura da tela) e tamanho |
| `[present]` | `overlay` | `0` | Teste: `1` mostra um quadro no canto com o desgaste médio do caminhão e de cada carreta e os pneus estourados |
| `[blowout]` | `enabled` | `1` | Liga os estouros (`0` desliga) |
| `[blowout]` | `start_wear`, `km_at_100`, `curve` | `0.60`, `300`, `3` | Desgaste a partir do qual um pneu pode estourar, distância média até estourar a 100% e quão rápido a chance cresce |
| `[blowout]` | `test_after_s` | `0` | Teste: estoura um pneu depois de tantos segundos dirigindo (vale só naquela sessão). Escolha o pneu com `test_unit` (`truck` ou `trailer`) e `test_wheel` (`-1` = sorteado) |
| `[effect]` | `enabled` | `1` | Efeito do estouro no caminhão (puxada, peso e limite de velocidade) e som (`0` desliga) |
| `[effect]` | `pull`, `wobble` | `0.12`, `0.015` | Desvio na direção e vibração, no teclado ou sem force feedback |
| `[effect]` | `ffb` | `1` | Volante com force feedback: a puxada vira força no volante (`0` = desvio na direção) |
| `[effect]` | `ffb_pull`, `ffb_kick`, `ffb_wobble` | `0.25`, `0.6`, `0.08` | Força da puxada, do tranco e da vibração no volante (fração da força máxima) |
| `[effect]` | `ffb_invert`, `ffb_device` | `0`, vazio | Inverte o sentido da força; parte do nome do volante, se o plugin não o escolher sozinho |
| `[effect]` | `limit_truck_kmh`, `limit_trailer_kmh` | `60`, `70` | Limite de velocidade com pneu do caminhão ou só da carreta estourado |
| `[effect]` | `sound`, `sound_volume`, `flap_volume` | `1`, `0.8`, `0.6` | Som do estouro e do pneu batendo (volume do plugin, de `0` a `1`; não acompanha o volume do jogo) |
| `[effect]` | `visual` | `1` | Pneu rasgado na roda estourada |
| `[tires]` | `capture` | `0` | Diagnóstico: grava os dados das rodas desenhadas num arquivo `tire_capture_*.txt` |
| `[tires]` | `tint_test` | `0` | Diagnóstico: pinta os pneus trocados de magenta |
| `[present]`, `[tires]` | `probe`, `hook`, `log` | `1` | Precisam ficar em `1` para o plugin funcionar |

Um INI criado por uma versão anterior não tem as chaves novas: valem os padrões (estouros e aviso ligados). Para mudar uma delas, acrescente a chave na seção.

## Limitações conhecidas

- Uma roda isolada da carreta, vista pela janela da cabine, pode ficar com a aparência original.
- Um caminhão idêntico ao seu, **parado** colado no retrovisor, pode aparecer com o pneu gasto no reflexo.
- Visto de muito longe, a linha branca de alguns Michelin (XZE) ainda aparece fraca.
- Em alguns modelos, uma linha clara fina no centro da banda some a partir de 14%.
- Logo depois de carregar, o pneu pode ficar original por alguns frames, até as versões gastas ficarem prontas.
- Estouro no **teclado**: o acelerador não é limitado (o jogo não deixa), só o freio a partir de 8 km/h acima do limite.
- Força no volante testada só no G923. Um volante que o jogo não leia pelo DirectInput, ou que o plugin não reconheça (veja `ffb_device`), fica com o desvio na direção, que num volante com force feedback é sentido para o lado contrário.
- O pneu estourado não fica mais baixo nem muda a física do jogo: o efeito vem só da direção, do acelerador e dos freios.
- O idioma do aviso é lido quando o jogo abre: se você trocar o idioma, reinicie o jogo.

## Problemas e feedback

Abra uma [issue](../../issues) com:
- o jogo (ETS2 ou ATS) e o arquivo `Documentos\<jogo>\TireWear\present_probe.log` (ele lista caminhos de arquivos do seu PC, que podem conter seu nome de usuário do Windows: revise antes de postar);
- o caminhão, a carreta e o pneu usados;
- o que aconteceu, de preferência com um print.

## Compilar

Requisitos: Visual Studio 2022 com C++ e CMake 3.20 ou mais novo.

1. Baixe o **SCS Telemetry SDK** (1.14 ou mais novo) na [documentação de modding da SCS](https://modding.scssoft.com/wiki/Documentation/Engine/SDK/Telemetry) e extraia de modo que exista `third_party\scs_sdk\include\scssdk.h`. Os headers da SCS não são distribuídos aqui.
2. Num *Developer PowerShell for VS 2022*:
   ```bash
   cmake -S . -B build -G "Visual Studio 17 2022" -A x64
   ```
   ```bash
   cmake --build build --config Release
   ```
3. A DLL fica em `build\out\ets2_tire_wear.dll`. Para montar o zip de release:
   ```bash
   powershell -File tools\package.ps1 -Version 0.3.0-beta
   ```

### Estrutura

| Pasta | Conteúdo |
|---|---|
| `src/` | Plugin: telemetria e input (`plugin.cpp`), hooks D3D11/DXGI (`present_probe.cpp`, `tire_probe.cpp`), transformação de desgaste e pneu rasgado (`wear_transform.cpp`), separação jogador × tráfego (`player_tracker.cpp`), estouro (`blowout.cpp`), efeito no caminhão (`effect.cpp`), som (`tire_audio.cpp`), aviso (`notice.cpp`) e desenho na tela (`overlay.cpp`) |
| `catalog/` | Por jogo (`ets2/`, `ats/`): hashes das texturas de pneu e retângulo da banda de cada uma (embutidos na DLL) |
| `package/` | LEIA-ME do zip |
| `tools/` | Ferramentas de desenvolvimento: `wearpreview` (prévias e benchmark da transformação, inclusive do pneu rasgado), `trackertest` (testa a separação com capturas), `tirecat` (gera o catálogo a partir dos arquivos extraídos do jogo), `blowoutsim` (simula estouros e efeito fora do jogo), `tiresound` (gera o som num WAV), `noticepreview` (prévia do aviso) |
| `third_party/` | MinHook 1.3.4 (BSD-2), Dear ImGui 1.92.9b (MIT) e a fonte Rubik (SIL OFL 1.1) |

## Apoie meu trabalho

Faço plugins de realismo gratuitos e de código aberto para Euro Truck Simulator 2, feitos e mantidos no meu tempo livre. Se você gosta deles e quer apoiar novas versões e atualizações, você pode ajudar:

[![Buy Me a Coffee](https://img.shields.io/badge/Buy%20Me%20a%20Coffee-apoie-yellow?logo=buymeacoffee&logoColor=white)](https://buymeacoffee.com/leonardoarouck)
[![PIX](https://img.shields.io/badge/PIX-Apoiar%20via%20Pix-32BCAD?style=for-the-badge&logo=pix&logoColor=white)](#apoie-meu-trabalho)

**Pix:** `fe1a4c11-6bb5-4844-b687-1f9143512862`

## Licenças e créditos

- Este projeto: [MIT](LICENSE). O mesmo arquivo traz os avisos das duas bibliotecas embutidas na DLL: [MinHook](https://github.com/TsudaKageyu/minhook) (BSD 2-Clause) e [Dear ImGui](https://github.com/ocornut/imgui) (MIT).
- Fonte do aviso: [Rubik](https://github.com/googlefonts/rubik), embutida na DLL, sob a [SIL Open Font License 1.1](third_party/rubik/OFL.txt).
- Euro Truck Simulator 2, American Truck Simulator, Goodyear e Michelin são marcas dos seus donos. Este projeto não tem ligação com a SCS Software nem com os fabricantes de pneus, e não distribui arquivos do jogo.
