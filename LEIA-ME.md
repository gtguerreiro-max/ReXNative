<div align="center">

# ReXNative

**Um renderer Direct3D 12 nativo para jogos de Xbox 360 recompilados com o ReXGlue SDK, com DLAA,
DLSS, DLSS 5 e FSR 4, e uma ferramenta que o adapta sozinha a um jogo novo.**

![Direct3D 12](https://img.shields.io/badge/Direct3D-12-0078D6?logo=windows&logoColor=white)
![DLSS](https://img.shields.io/badge/NVIDIA-DLAA%20%7C%20DLSS%20%7C%20DLSS%205-76B900?logo=nvidia&logoColor=white)
![FSR](https://img.shields.io/badge/AMD-FSR%204%20%7C%203.1-ED1C24?logo=amd&logoColor=white)
![Steam Deck](https://img.shields.io/badge/Steam%20Deck-Proton-1A9FFF?logo=steamdeck&logoColor=white)
![ReXGlue](https://img.shields.io/badge/ReXGlue-v0.11.0-555555)
![Ficheiros dos jogos](https://img.shields.io/badge/ficheiros%20dos%20jogos-n%C3%A3o%20inclu%C3%ADdos-lightgrey)

[English](README.md)

</div>

Um port [ReXGlue](https://github.com/rexglue/rexglue-sdk) corre o código PowerPC do próprio jogo
recompilado para x64, e desenha-o através de uma emulação da GPU da Xbox 360 (Xenos: command
buffers, EDRAM, resolves). **O ReXNative desenha o jogo em Direct3D 12.** Liga-se às chamadas do
jogo à biblioteca Direct3D do XDK (draws, clears, resolves, Swap), lê o estado que o device D3D
do jogo guarda e desenha cada chamada de forma nativa. Fica espaço para o que a consola nunca
teve: upscalers modernos, MSAA, HDR, um menu de definições por cima do jogo.

Um só renderer serve todos os jogos. O que muda de jogo para jogo, umas dezenas de endereços e
offsets de estruturas, é encontrado no próprio código recompilado do jogo por uma ferramenta, que
também pega num jogo tal como vem (um arquivo, uma imagem de disco, um pacote da Xbox Live) e
deixa um executável compilado que corre.

> Este repositório não tem ficheiros, código gerado nem dados de nenhum jogo. É preciso ter uma
> cópia própria de cada jogo.

- [Funcionalidades](#funcionalidades) · [Jogos](#jogos) · [Começar](#começar) ·
  [Como funciona](#como-funciona) · [Num projeto ReXGlue](#num-projeto-rexglue) ·
  [Teclas e definições](#teclas-e-definições) · [Depurar um jogo](#depurar-um-jogo) ·
  [Organização](#organização-do-repositório) · [Próximos passos](#próximos-passos) ·
  [Licença](#licença-e-créditos)

## Funcionalidades

- **Upscaling e anti-aliasing na cena 3D:** NVIDIA DLAA, DLSS Super Resolution e DLSS 5 (neural
  rendering) em placas RTX; AMD FSR 4 e 3.1 nas outras, a Steam Deck incluída; frame generation
  do FSR.
- **Imagem:** MSAA até 8x, filtragem anisotrópica, FSR 1 no ecrã, saída HDR10, as rampas de gama
  da consola, ritmo dos frames no ecrã.
- **Um HUD de definições por cima do jogo** (F1, ou Back + Start no comando), gravado no ficheiro
  de configuração do jogo. A página Upscaler muda o upscaler, o tipo (automático, DLSS ou FSR), a
  versão do FSR e o modo (anti-aliasing nativo a ultra performance) com o jogo a correr, e diz o
  que está a correr ("AMD FSR 4.1.2 Quality, 853x480 to 1280x720").
- **Menos engasgos a compilar shaders:** os pipelines que o jogo criou em sessões anteriores são
  compilados outra vez em segundo plano no arranque.
- **O renderer original a uma tecla:** F9 troca entre o ReXNative e a emulação da GPU Xenos do
  ReXGlue com o jogo a correr, para os comparar.
- **Windows e Steam Deck** (com Proton, através do vkd3d-proton).

## Jogos

| Jogo | No PC | Steam Deck |
|---|---|---|
| From Dust | joga-se: menus, níveis, HUD, 60 FPS | 60 FPS ([FromDustDeck](https://github.com/gtguerreiro-max/FromDustDeck)) |
| Iron Brigade | joga-se: a introdução e o vídeo, a primeira missão com o cenário | os seus 30 FPS ([IronBrigadeRecomp](https://github.com/gtguerreiro-max/IronBrigadeRecomp)) |
| Jet Set Radio (TU 1) | joga-se: menus, a introdução da história, o primeiro nível | ainda não testado |
| Dungeons & Dragons: Chronicles of Mystara | joga-se: menus, escolha de capítulo, o primeiro capítulo, 60 FPS | ainda não testado |
| Super Meat Boy (TU 1) | joga-se: menus, o mapa do mundo, o nível 1-1 | ainda não testado |
| Minecraft: Xbox 360 Edition (TU 80) | joga-se: menus, os modelos de mundo do update, um mundo novo | ainda não testado |
| Castlevania: Symphony of the Night (TU 2) | joga-se: o menu com o texto, o nome, o prólogo | ainda não testado |
| Orcs Must Die! (TU 1) | joga-se o primeiro nível, 30 FPS | ainda não testado |
| Double Dragon Neon | joga-se a primeira missão | ainda não testado |
| Brothers - A Tale of Two Sons | menu e abertura, desenhados como no renderer original | ainda não testado |
| Bionic Commando Rearmed (TU 2) | chega ao mapa das missões | ainda não testado |
| Halo: Combat Evolved Anniversary (TU 2) | menu da campanha e a cena de abertura; algumas superfícies escuras ou às riscas | ainda não testado |
| Spelunky (TU 1) | menus, sem texto (também no renderer original) | ainda não testado |
| Perfect Dark (TU 3) | chega aos níveis, mas desenha-os partidos (o renderer original não arranca) | ainda não testado |
| 3D Ultra Minigolf Adventures | passa o arranque | ainda não testado |
| Project Gotham Racing 4 | a origem do renderer, com cópia própria ([pgr4-native](https://github.com/shaunsingh/pgr4-native)) | |

TU: o title update instalado (sempre o mais recente). From Dust, Iron Brigade e Jet Set Radio
foram verificados no ReXGlue v0.11 a 2026-11-16; os outros resultados são das builds ReXGlue
v0.10 do mesmo dia, e os da Steam Deck de builds v0.10. O que cada jogo precisou está em
[docs/NOTES.md](docs/NOTES.md) (em inglês).

## Começar

**Requisitos:** Windows 10 ou 11; Python 3.10+ (a ferramenta não tem dependências); Visual Studio
2022 ou as Build Tools com o workload de C++; LLVM 20 (clang++ e lld: o ReXGlue v0.11 precisa do
clang 20 ou mais recente); CMake, Ninja e git; o Vulkan SDK (o ReXGlue também compila o driver
Vulkan); o NVIDIA DLSS SDK e os cabeçalhos do DLSS 5 (da NVIDIA). Para adaptar o renderer a um
jogo novo, a ferramenta precisa ainda de um jogo que já conheça (uma *referência*), com o código
gerado pelo ReXGlue e o `default.xex`: From Dust, Iron Brigade, Project Gotham Racing 4, ou
qualquer jogo que já tenha adaptado.

**1. Compilar o SDK.** O ReXGlue v0.11.0, fixo, com os patches de que o renderer precisa
([sdk/README.md](sdk/README.md)), e o AMD FidelityFX SDK v2.4.0:

```powershell
scripts\build-sdk.ps1 -Llvm C:\caminho\para\LLVM-20\bin
```

**2. Dizer à ferramenta onde estão as coisas.** Copiar `ports.example.json` para `ports.json` e
pôr os caminhos dos jogos de referência. Depois:

```powershell
$env:REXNATIVE_LLVM        = "C:\caminho\para\LLVM-20\bin"
$env:REXNATIVE_REXGLUE     = "$PWD\build\sdk-install\bin\rexglue.exe"
$env:REXNATIVE_SDK_INSTALL = "$PWD\build\sdk-install"
$env:REXNATIVE_SDK_SRC     = "$PWD\build\rexglue-sdk"
$env:REXNATIVE_FIDELITYFX  = "$PWD\build\fidelityfx-sdk"
$env:REXNATIVE_DLSS_SDK    = "C:\caminho\para\DLSS"
$env:REXNATIVE_DLSS_NR     = "C:\caminho\para\dlss_nr"
```

**3. Preparar o jogo.** A partir de um arquivo, uma pasta, um pacote da Xbox Live (STFS) ou uma
imagem de disco:

```powershell
python -m rexnative prepare "D:\Jogos\O Meu Jogo.zip" --work D:\rexnative-work --title-update --project --lang pt
```

Isto extrai o jogo, descarrega e instala o title update mais recente (da lista do XboxUnity,
verificado contra ela), corre o codegen do ReXGlue até ficar limpo, gera a camada do jogo
(`layer/report.md` diz o que encontrou, como e com que certeza), escreve um projeto ReXGlue com o
ReXNative e o HUD de definições, compila-o, encontra as funções que o codegen não viu e compila
outra vez. O jogo fica em `D:\rexnative-work\<jogo>\run\`: é só correr o executável. Sem
`--lang pt` (ou `REXNATIVE_LANG=pt`), os relatórios saem em inglês.

Outros comandos: `info <xex>` (título, versão, bibliotecas do XDK), `title-update <work>` (os
title updates da lista, sem descarregar nada), `project <work>/<jogo> --build` (recompilar um
jogo), `survey --work <work>` (uma tabela dos jogos preparados), `adapt` (só a camada, para um
projeto ReXGlue próprio), `scene` (a superfície da cena a partir de um dump de resolves),
`validate <port>` (regenerar um port conhecido e comparar), `extract`, `neutral`. `python -m
rexnative <comando> --help` mostra as opções de cada um.

## Como funciona

O que muda de jogo para jogo é pouco:

| O quê | Como o ReXNative o encontra |
|---|---|
| Os endereços das funções D3D do XDK que liga (Swap, Draw*, Clear, Resolve, segmentos do push buffer, shader load, e cerca de 100 para o trace) | compara o código gerado pelo ReXGlue do jogo com o de uma referência |
| Os offsets dos campos do device D3D do jogo | votos nas instruções alinhadas das funções emparelhadas |
| Que Resolve ligar e onde estão os argumentos (o LTCG faz inline do wrapper e tira argumentos) | segue os usos de cada argumento nas duas listagens |
| A cor de clear por omissão do Resolve | o padrão do código: um float (XDK 6132) ou um vetor de 16 bytes (XDK 20353) |
| O comando que o HUD lê | o XInputGetState do jogo (o wrapper de três instruções do XDK, encontrado pela forma) |
| A superfície da cena para os upscalers | em runtime, a partir dos resolves de um frame (`rexnative scene`) |
| O comportamento do motor (um segundo device D3D, valores por omissão) | escolha tua, no `overrides.json` |

A ferramenta escreve tudo num cabeçalho, `game_layer.h`, e numa lista de hooks,
`d3d_surface.inc` ([docs/GAME_LAYER.md](docs/GAME_LAYER.md)). O renderer não lê mais nada sobre o
jogo.

O matching trabalha sobre o C++ que o ReXGlue gera (cada instrução PowerPC fica lá como
comentário), sem desassemblador. Para cada função de uma referência tenta, por ordem de
confiança: a mesma listagem; a mesma com os números mascarados; a única dentro da biblioteca D3D;
a função que uma já emparelhada chama no mesmo sítio (a ideia dos XREF do XbSymbolDatabase do
Cxbx-Reloaded); a vizinha de uma emparelhada (a biblioteca mantém a ordem dos ficheiros objeto);
a que chama as mesmas funções; uma listagem parecida; e um wrapper público que o compilador fez
desaparecer. A referência escolhe-se pelo `default.xex`: primeiro a mesma versão da biblioteca
D3D, depois LTCG ou não, depois a build do XDK mais próxima; as outras preenchem falhas e
confirmam. Regenerar From Dust e Iron Brigade sem as suas próprias camadas deu os mesmos hooks e
offsets do device que as feitas à mão, e encontrou erros nelas
([validação](docs/NOTES.md#validating-the-adapter)).

Um jogo recompilado precisa ainda de coisas que o codegen do ReXGlue não encontra sozinho, e que
o `prepare` e o `project` lhe dão como hints: inícios de funções só alcançados por ponteiros,
jump tables lidas curtas, os endereços do setjmp e do longjmp, fibers da XAPI, imports em falta
([docs/NOTES.md](docs/NOTES.md)).

## Num projeto ReXGlue

Para um jogo que já é um projeto ReXGlue, gerar a camada:

```bash
python -m rexnative adapt --generated caminho/do/jogo/generated/default --xex caminho/do/jogo/default.xex --name meujogo --title "O Meu Jogo" --out results/meujogo
```

juntar o renderer à build:

```cmake
include(caminho/para/ReXNative/renderer/rexnative.cmake)
rexnative_add(meujogo_common         # a biblioteca do host (rexglue_setup_host_library)
    EXECUTABLE      meujogo
    LAYER_DIR       ${CMAKE_CURRENT_SOURCE_DIR}/results/meujogo
    REXGLUE_SDK_SRC caminho/para/rexglue-sdk
    FIDELITYFX_SDK  caminho/para/FidelityFX-SDK
    DLSS_SDK        caminho/para/DLSS
    DLSS_NR         caminho/para/dlss_nr)
```

e chamá-lo na app do jogo (uma `rex::app::TitleApp`): `rexnative::host::Configure()` no
`OnConfigure`; `rexnative::host::Attach(*this)` e `rexnative::hud::Initialize()` no `OnInit`;
`rexnative::hud::Shutdown()` e `rexnative::host::Detach()` no `OnShutdown`
(`renderer/src/host.h`, `renderer/src/hud/hud.h`). O `rexnative project` escreve uma app assim.

Para os upscalers, medir a cena uma vez: correr com `--rexnative_resolve_dump=<dir>`, criar
`<dir>/dump.request` numa cena 3D, correr `python -m rexnative scene <dir>/f<N>-resolves.jsonl`,
pôr o resultado em `results/meujogo/overrides.json` e voltar a correr o `adapt`.

## Teclas e definições

| Tecla | Comando | |
|---|---|---|
| F1 | Back + Start | o HUD de definições |
| F9 | | troca entre o ReXNative e a emulação da GPU Xenos do ReXGlue |
| F7 | | liga ou desliga o upscaler |
| F8 | | mostra as entradas do upscaler |

As teclas também são definições (`bind_rexnative_hud`, `bind_renderer_switch`, `bind_upscaler`,
`bind_upscaler_debug`). Todas as opções do renderer são definições `rexnative.*`, na secção
`[rexnative]` do ficheiro de configuração do jogo (`run/<jogo>.toml`) ou na linha de comandos;
`--rexnative=false` arranca o jogo com o renderer do ReXGlue. Os nomes antigos
(`--rexnative_upscaler`, os `pgr4_native_*` e `pgr4_d3d_*` do PGR4Native) continuam a funcionar
como aliases.

## Depurar um jogo

Arrancar o jogo para cada experiência custa minutos (logótipos, introdução, menus). Em vez disso,
`scripts\live.ps1 start -Dir <pasta do jogo> -Exe <exe> -Route "<teclas>"` arranca-o uma vez numa
janela de 1280x720 e leva-o até ao problema; depois cada experiência demora segundos sobre o jogo
a correr:

- `shot`: uma captura da janela; `log`: o log do jogo; `stop`;
- `debug "<máscara>"`: os interruptores do renderer (depth, stencil, cull, blend, alpha test
  desligados, saltar uma gama de draws);
- `snap`: os render targets depois de certos shaders (`python scripts\snaps.py` passa-os a
  imagens); `drawlog`: os draws de um frame; `trace`: o push buffer do jogo num frame.

Com `start -RenderDoc` (a pasta do RenderDoc em `REXNATIVE_RENDERDOC`), `capture` tira uma
captura RenderDoc do frame seguinte, e `scripts\rdoc.ps1` consulta-a com o jogo fechado:
`actions` (cada draw, o número de índices e os targets), `history -X -Y` (que draws tocaram num
pixel e o que falhou), `inputs -Event` (as texturas de um draw em PNG), `buffer -Offset -Size` (a
cópia da memória do jogo no renderer), `mesh`, `disasm`, `cbuffers -Event`, `save`. Só uma nova
build obriga a arrancar outra vez. [docs/NOTES.md](docs/NOTES.md#recorded-command-buffers)
mostra como se encontrou assim o cenário em falta do Iron Brigade.

## Organização do repositório

```
renderer/src/native/                  o renderer (igual para todos os jogos)
renderer/src/rexnative_d3d_hooks.cpp  os hooks do D3D do jogo e o trace dos frames
renderer/src/hud/                     o HUD de definições
renderer/src/host.{h,cpp}             o lado do renderer na app ReXGlue: janela, input, memória
renderer/src/glue_defaults.cpp        funções do lado do jogo, para um jogo sem nada de especial
renderer/rexnative.cmake              rexnative_add()
rexnative/                            a ferramenta (python -m rexnative)
sdk/                                  o ReXGlue SDK de que o renderer precisa: lock e patches
scripts/build-sdk.ps1                 descarrega, aplica os patches, compila e instala esse SDK
scripts/live.ps1, snaps.py            um jogo a correr para experiências, snapshots em imagens
scripts/rdoc.ps1, rdoc.py             consultas a capturas RenderDoc
docs/GAME_LAYER.md                    o que o game_layer.h contém
docs/NOTES.md                         o que cada jogo precisou, e como se descobriu
```

## Próximos passos

- Encontrar a superfície da cena em runtime, para os upscalers funcionarem sem a medir.
- Encontrar o passo HDR antes do tone mapping do jogo, para o DLSS 5.
- Os problemas em aberto da tabela: os níveis partidos do Perfect Dark, o texto em falta do
  Spelunky, as superfícies escuras e às riscas do Halo.
- Memexport: um jogo cuja imagem precise dele, e não esperar pela GPU quando nada no CPU lê o
  que ele escreveu.
- A Steam Deck no ReXGlue v0.11, e os jogos da tabela nela.
- Compilar um jogo do princípio ao fim a partir do `scripts\build-sdk.ps1` numa máquina limpa.
- Cada jogo novo torna-se uma referência e facilita o seguinte.

Feito: o renderer compilado a partir deste repositório para From Dust e Iron Brigade
(2026-11-14); command buffers gravados, memexport, pipelines compilados antecipadamente, e mais
treze jogos do princípio ao fim (2026-11-15 e 16); a passagem para o ReXGlue v0.11.0
(2026-11-16).

## Licença e créditos

MIT ([LICENSE](LICENSE)). O renderer é o do pgr4-native (MIT), aqui tornado universal, com código
portado do ReXGlue SDK e do Xenia (BSD-3-Clause); ver
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). O NVIDIA DLSS e o AMD FidelityFX vêm dos
respetivos SDKs, com as suas licenças, e não estão neste repositório. Xbox 360 é uma marca da
Microsoft; este projeto não tem ligação à Microsoft, à NVIDIA, à AMD nem às editoras dos jogos.
