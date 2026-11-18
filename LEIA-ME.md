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
  [Teclas e definições](#teclas-e-definições) · [Steam Deck](#steam-deck) · [Depurar um jogo](#depurar-um-jogo) ·
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
  que está a correr ("AMD FSR 4.1.2 Quality, 853x480 to 1280x720"). A página DLSS 5 escolhe um
  aspeto, um conjunto das definições do DLSS 5 por tipo de jogo (Cinematic, Cinematic com duas
  passagens, Natural, Stylized, Cel-shaded / 2D, Strong, ou Custom), com a intensidade por cima;
  o `project` dá a cada jogo o aspeto que lhe convém (`rexnative/dlss5.py`: os jogos cel-shaded,
  anime e 2D ficam com Cel-shaded, o 3D de desenho animado ou pintado com Stylized, os de corridas
  com duas passagens, os realistas com Cinematic), segundo o que a comunidade de mods encontrou (o
  estilo Cinematic e duas passagens em jogos antigos de consola; a arte cel-shaded só fica mais
  nítida; uma força de cor 0 mantém as cores do jogo).
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
| Jet Set Radio (TU 1) | joga-se: menus, a introdução da história, o primeiro nível | 30 FPS |
| Dungeons & Dragons: Chronicles of Mystara | joga-se: menus, escolha de capítulo, o primeiro capítulo, 60 FPS | 60 FPS |
| Super Meat Boy (TU 1) | joga-se: menus, o mapa do mundo, o nível 1-1 | 60 FPS |
| Minecraft: Xbox 360 Edition (TU 80) | joga-se: menus, os modelos de mundo do update, um mundo novo | 60 FPS |
| Castlevania: Symphony of the Night (TU 2) | joga-se: o menu com o texto, o nome, o prólogo | 60 FPS |
| Orcs Must Die! (TU 1) | joga-se: título, menus, a história da campanha e o primeiro nível com o HUD, 30 FPS (a compilação pôs muitos desenhos inline, fora das funções ligadas: `rexnative.inline_draws`, que se liga sozinho) | 30 FPS |
| Double Dragon Neon | joga-se a primeira missão | 60 FPS |
| Brothers - A Tale of Two Sons | menu e abertura, desenhados como no renderer original | 30 FPS (renderer do ReXGlue) |
| Bionic Commando Rearmed (TU 2) | a introdução a 60 FPS, o mapa das missões | 60 FPS |
| Midnight Club: Los Angeles - Complete Edition | joga-se: a cidade, os carros (de command buffers gravados), 30 FPS | 30 FPS |
| Dark Souls II (TU 10) | joga-se: menus, um jogo novo até Things Betwixt, 60 FPS (os 30 da consola com `rexnative.menu_fps = 30`); o vídeo do prólogo sai estragado (também no renderer do ReXGlue: a descodificação do WMV) | 30 FPS (Things Betwixt) |
| Halo: Combat Evolved Anniversary (TU 2) | menu da campanha e a cena de abertura; algumas superfícies escuras ou às riscas | 60 FPS (a introdução) |
| Spelunky (TU 1) | menus, sem texto (também no renderer do ReXGlue, e no Xenia: [game-compatibility#97](https://github.com/xenia-project/game-compatibility/issues/97)) | 60 FPS, sem texto |
| Perfect Dark (TU 3) | chega aos níveis, mas desenha-os partidos (o renderer do ReXGlue já chega aos menus, para comparar) | 60 FPS (menus) |
| 3D Ultra Minigolf Adventures | passa o arranque | 60 FPS |
| Project Gotham Racing 4 | a origem do renderer, com cópia própria ([pgr4-native](https://github.com/shaunsingh/pgr4-native)) | |

TU: o title update instalado (sempre o mais recente). Todos os resultados são de builds ReXGlue
v0.11 de 2026-11-16 e 17; os da Steam Deck de `python -m rexnative deck <work>/<jogo>... --steam
--try 70` (a velocidade do ecrã a que cada jogo chega sozinho em 70 segundos, no GE-Proton; ver
[Steam Deck](#steam-deck)). O que cada jogo precisou está em [docs/NOTES.md](docs/NOTES.md) (em
inglês).

## Começar

**Requisitos:** Windows 10 ou 11; Python 3.10+ (a ferramenta não tem dependências; os scripts
de depuração `snaps.py` e `resolves.py` precisam do Pillow e do numpy); Visual Studio
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

Os DLLs do DLSS e do DLSS 5 da NVIDIA e do FidelityFX da AMD (413 MB) ficam uma só vez para todos
os jogos, em `<work>\common` (`D:\rexnative-work\common`), e não no `build\` e no `run\` de cada
jogo; o renderer procura-os primeiro ao lado do executável (um port que os tenha lá continua a
funcionar), depois em `..\common` e `..\..\common` (o `--rexnative_common_dir` indica outra
pasta). Na Steam Deck ficam em `~/Games/ReXNative/common`.

Outros comandos: `info <xex>` (título, versão, bibliotecas do XDK), `title-update <work>` (os
title updates da lista, sem descarregar nada), `dlc <work>/<jogo> <pacotes, pastas ou arquivos>`
(o conteúdo descarregável, onde o jogo o encontra: `run/userdata/0000000000000000/<title id>/00000002`
com os ficheiros de cabeçalho; o `deck` leva-o também), `project <work>/<jogo> --build` (recompilar
um jogo; os hooks do próprio jogo em `src/game_*.cpp` entram na compilação), `survey --work <work>` (uma tabela dos jogos preparados), `deck <work>/<jogo>...` (jogos
compilados para uma Steam Deck: [Steam Deck](#steam-deck)), `adapt` (só a camada, para um
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
| A superfície da cena para os upscalers | aprendida em runtime a partir dos resolves do jogo; medida uma vez (`rexnative scene`) para uma cena sem MSAA nem formato HDR |
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

Os upscalers encontram sozinhos a cena 3D do jogo: o renderer aprende-a a partir dos resolves do
jogo (o log diz `ReXNative scene: learned at frame ...`;
[docs/GAME_LAYER.md](docs/GAME_LAYER.md#the-scene-at-run-time)). Uma cena sem MSAA nem formato
HDR não se encontra assim: medi-la uma vez correndo com `--rexnative_resolve_dump=<dir>`, criando
`<dir>/dump.request` numa cena 3D e correndo `python -m rexnative scene
<dir>/f<N>-resolves.jsonl`; pôr o resultado em `results/meujogo/overrides.json` e voltar a correr
o `adapt`.

## Teclas e definições

| Tecla | Comando | |
|---|---|---|
| F1 | Back + Start (na Deck: View + Menu) | o HUD de definições |
| F9 | | troca entre o ReXNative e a emulação da GPU Xenos do ReXGlue (não no Proton) |
| F7 | | liga ou desliga o upscaler |
| F8 | | mostra as entradas do upscaler |

As teclas também são definições (`bind_rexnative_hud`, `bind_renderer_switch`, `bind_upscaler`,
`bind_upscaler_debug`). Todas as opções do renderer são definições `rexnative.*`, na secção
`[rexnative]` do ficheiro de configuração do jogo (`run/<jogo>.toml`) ou na linha de comandos;
`--rexnative=false` arranca o jogo com o renderer do ReXGlue. `rexnative.menu_fps` limita a
velocidade (60 por omissão, para os menus feitos para os 60 Hz de uma consola manterem a
velocidade num ecrã mais rápido; 30 para um jogo que a consola corria a 30; 0 sem limite). Os
nomes antigos (`--rexnative_upscaler`, os `pgr4_native_*` e `pgr4_d3d_*` do PGR4Native)
continuam a funcionar como aliases.

Um jogo cuja compilação pôs desenhos em código inline, fora das funções D3D ligadas (o título,
os menus, os carregamentos e o HUD do Orcs Must Die!), mostra-os a preto. Com
`rexnative.inline_draws` o ReXNative percorre o push buffer que o jogo escreveu entre duas
chamadas ligadas, como percorre um command buffer gravado, e desenha o que lá encontra. O
`rexnative.inline_draws_auto` (ligado por omissão) experimenta-o sozinho quando 30 frames
seguidos chegam às funções ligadas com no máximo 2 chamadas de desenho enquanto o jogo escreve no
push buffer, e só o mantém se o percurso encontrar desenhos (um vídeo ou um ecrã de carregamento
parecem iguais e não os têm).
`rexnative.auto_handover = 30` é a outra saída: ao fim de 30 frames em que o ReXNative vê no
máximo uma chamada de desenho, o renderer do ReXGlue desenha até as chamadas do jogo voltarem,
como faria o F9. Fica desligado no Wine, tal como o F9: no vkd3d-proton a janela não pode passar
da cadeia de troca de um renderer para a do outro (o Present do driver Xenos fica à espera para
sempre).

## Steam Deck

Um jogo compilado no PC corre na Steam Deck com o Proton (o D3D12 através do vkd3d-proton):

```powershell
python -m rexnative deck D:\rexnative-work\<jogo> --host deck@192.168.1.10 --key ~\.ssh\deck_key --steam --try 70
```

copia-o por SSH para `~/Games/ReXNative/<jogo>/` na Deck (o executável e as DLLs sem as da
NVIDIA, os ficheiros do jogo, o title update; só o que mudou é enviado de novo), com o
[deck/](deck/)`play.sh` (arranca-o com o Proton: o GE-Proton, senão o da Valve, com um prefixo
que todos os jogos partilham, `~/Games/ReXNative/prefix`; os DLLs do FidelityFX da AMD ficam em
`~/Games/ReXNative/common`, uma vez para todos), o `install.sh` e o `steam_shortcuts.py`. O `--steam` junta cada jogo à biblioteca do
Steam como jogo não-Steam (o Steam é fechado entretanto e volta a arrancar); o `--try SEGUNDOS`
arranca cada um a partir do Steam, lê o log e tira uma captura ao fim desse tempo, para
`<work>/<jogo>/deck/`, e escreve o `deck-report.md`. A Deck precisa de SSH com chave (o
`REXNATIVE_DECK` e o `REXNATIVE_DECK_KEY` podem substituir o `--host` e o `--key`).

As definições do jogo na Deck são as do PC com o ecrã da Deck (1280x800, ecrã inteiro) e o FSR 4
da AMD como antialiasing da cena à resolução do próprio jogo (Native AA; o FSR 4 do GE-Proton, ou
o FSR 3.1 sem ele): nos dois Overlord manteve os 30 FPS e ficou melhor do que o MSAA da consola.
As definições mudadas na Deck (o HUD: View + Menu) ficam nas instalações seguintes. O
`<work>/<jogo>/deck.toml` junta ou substitui definições só para a Deck, como `[rexnative]
enabled = false` para um jogo que o renderer do ReXGlue desenha melhor (um renderer para a sessão
inteira: o F9 e o `auto_handover` ficam desligados no Proton). Na Deck, um `deck.env` ao lado do
`play.sh` dá variáveis ao Proton (`PROTON_LOG=1`), e o `REXNATIVE_PROTON` escolhe outro Proton.

O `python -m rexnative art <work>/<jogo> <captura.png>` faz as imagens do jogo no Steam (capa da
biblioteca, capa larga, faixa, ícone) a partir de uma captura dele, de preferência do ecrã-título
(`live.ps1 frame` ou `shot`, ou um `deck/try-*.png` do `--try`), para `<work>/<jogo>/deck/art/`,
que o `deck` seguinte envia e dá ao Steam com o atalho.

## Depurar um jogo

Arrancar o jogo para cada experiência custa minutos (logótipos, introdução, menus). Em vez disso,
`scripts\live.ps1 start -Dir <pasta do jogo> -Exe <exe> -Route "<teclas>"` arranca-o uma vez numa
janela de 1280x720 e leva-o até ao problema; depois cada experiência demora segundos sobre o jogo
a correr:

- `shot`: uma captura da janela; `frame`: o mesmo tirado pelo próprio renderer, sem precisar da
  janela à frente; `log`: o log do jogo; `stop`;
- `debug "<máscara>"`: os interruptores do renderer (depth, stencil, cull, blend, alpha test
  desligados, saltar uma gama de draws);
- `snap`: os render targets depois de certos shaders (`python scripts\snaps.py` passa-os a
  imagens); `drawlog`: os draws de um frame; `trace`: o push buffer do jogo num frame;
- no `-Route`, `pad:A+START:200` carrega nos botões do comando do jogo pelo ficheiro de controlo
  (sem precisar de foco, numa camada com XInputGetState).

Com `start -RenderDoc` (a pasta do RenderDoc em `REXNATIVE_RENDERDOC`), `capture` tira uma
captura RenderDoc do frame seguinte (pelo controlo de alvos do RenderDoc, por isso um aviso do
Windows com o foco não a impede), e `scripts\rdoc.ps1` consulta-a com o jogo fechado:
`actions` (cada draw, o número de índices e os targets), `history -X -Y` (que draws tocaram num
pixel e o que falhou), `inputs -Event` (as texturas de um draw em PNG), `buffer -Offset -Size` (a
cópia da memória do jogo no renderer), `mesh`, `disasm`, `cbuffers -Event`, `save`. Só uma nova
build obriga a arrancar outra vez. [docs/NOTES.md](docs/NOTES.md#recorded-command-buffers)
mostra como se encontrou assim o cenário em falta do Iron Brigade.

Na Steam Deck, `python scripts\deck_drive.py <work>\<jogo> <etiqueta> "<seg>:<item>,..." <fim>
[flags]` arranca um jogo instalado pelo Steam com flags extra e, nos segundos indicados, envia
teclas, tira capturas do renderer (`cap`), liga interruptores de draw-debug (`dbg<máscara>`) e lê
o consumo do APU (`pwr`); as capturas e o log voltam para `<work>\<jogo>\deck`.

## Organização do repositório

```
renderer/src/native/                  o renderer (igual para todos os jogos)
renderer/src/rexnative_d3d_hooks.cpp  os hooks do D3D do jogo e o trace dos frames
renderer/src/hud/                     o HUD de definições
renderer/src/host.{h,cpp}             o lado do renderer na app ReXGlue: janela, input, memória
renderer/src/glue_defaults.cpp        funções do lado do jogo, para um jogo sem nada de especial
renderer/rexnative.cmake              rexnative_add()
rexnative/                            a ferramenta (python -m rexnative)
deck/                                 os scripts de um jogo na Steam Deck (python -m rexnative deck)
sdk/                                  o ReXGlue SDK de que o renderer precisa: lock e patches
scripts/build-sdk.ps1                 descarrega, aplica os patches, compila e instala esse SDK
scripts/live.ps1, snaps.py            um jogo a correr para experiências, snapshots em imagens
scripts/rdoc.ps1, rdoc.py             consultas a capturas RenderDoc
scripts/rdoc_trigger.py               uma captura RenderDoc de um jogo a correr, sem foco
scripts/deck_drive.py                 um jogo na Steam Deck com horário (teclas, capturas, consumo)
docs/GAME_LAYER.md                    o que o game_layer.h contém
docs/NOTES.md                         o que cada jogo precisou, e como se descobriu
```

## Próximos passos

- Encontrar o passo HDR antes do tone mapping do jogo, para o DLSS 5.
- Os problemas em aberto da tabela: os níveis partidos do Perfect Dark, o texto em falta do
  Spelunky, as superfícies escuras e às riscas do Halo, os vídeos WMV do Dark Souls II e do Jet
  Set Radio.
- Uma reactive mask para os upscalers (a do próprio FSR, feita a partir da cena antes e depois
  dos desenhos transparentes), onde os vetores de movimento não podem estar certos: partículas, e
  os desenhos cuja malha o frame anterior não tinha.
- Memexport: um jogo cuja imagem precise dele, e não esperar pela GPU quando nada no CPU lê o
  que ele escreveu.
- Compilar um jogo do princípio ao fim a partir do `scripts\build-sdk.ps1` numa máquina limpa.
- Cada jogo novo torna-se uma referência e facilita o seguinte.

Feito: o renderer compilado a partir deste repositório para From Dust e Iron Brigade, e a
superfície da cena aprendida em runtime (2026-11-14); command buffers gravados, memexport,
pipelines compilados antecipadamente, e mais treze jogos do princípio ao fim (2026-11-15 e 16); a
passagem para o ReXGlue v0.11.0 e a Steam Deck com ele (2026-11-16); os carros do Midnight Club
e o Dark Souls II, os vetores de movimento e os cortes de câmara do Jet Set Radio, os desenhos
inline encontrados em runtime (2026-11-17).

## Licença e créditos

MIT ([LICENSE](LICENSE)). O renderer é o do pgr4-native (MIT), aqui tornado universal, com código
portado do ReXGlue SDK e do Xenia (BSD-3-Clause); ver
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). O NVIDIA DLSS e o AMD FidelityFX vêm dos
respetivos SDKs, com as suas licenças, e não estão neste repositório. Xbox 360 é uma marca da
Microsoft; este projeto não tem ligação à Microsoft, à NVIDIA, à AMD nem às editoras dos jogos.
