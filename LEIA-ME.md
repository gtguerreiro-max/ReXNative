<div align="center">

# ReXNative

**Um renderer Direct3D 12 nativo para jogos de Xbox 360 recompilados com o ReXGlue SDK, com
DLAA, DLSS e DLSS 5, e uma ferramenta que o adapta sozinha a um jogo novo.**

[English](README.md)

</div>

## O que é

Um port [ReXGlue](https://github.com/rexglue/rexglue-sdk) corre o código PowerPC do próprio jogo
recompilado para x64, e desenha através de uma emulação da GPU da Xbox 360 (Xenos): command
buffers, EDRAM, resolves. O ReXNative substitui esse desenho por um renderer próprio: liga-se às
chamadas do jogo à biblioteca Direct3D do XDK (draws, clears, resolves, Swap), lê o estado que o
device D3D do jogo guarda e desenha cada chamada directamente em D3D12. Sem emular a EDRAM, e com
espaço para o que a consola nunca teve:

- **NVIDIA DLAA, DLSS Super Resolution e DLSS 5** (neural rendering), ou AMD FSR, na cena 3D;
- MSAA até 8x, filtragem anisotrópica, FSR 1 no ecrã, frame generation, saída HDR10;
- ritmo dos frames no ecrã e um **HUD de definições** por cima do jogo (F1, ou Back + Start no
  comando: o ReXNative interceta o XInputGetState do jogo, que a ferramenta encontra, e esconde-lhe
  a combinação) que grava no ficheiro de configuração do jogo;
- corre em Windows e na Steam Deck (com Proton, através do vkd3d-proton).

O renderer vem do [pgr4-native](https://github.com/shaunsingh/pgr4-native) (Project Gotham
Racing 4) e foi portado à mão para From Dust
([FromDustDeck](https://github.com/gtguerreiro-max/FromDustDeck)) e Iron Brigade. O ReXNative é
esse renderer tornado **universal**: um só `src/native` para todos os jogos, e tudo o que muda de
jogo para jogo gerado por uma ferramenta.

## Como se adapta a um jogo

| O quê | Como o ReXNative o encontra |
|---|---|
| Endereços das funções D3D do XDK que liga (Swap, Draw*, Clear, Resolve, segmentos do push buffer, shader load, e ~100 para o trace) | compara o código gerado pelo ReXGlue do jogo com o de um port conhecido |
| Offsets dos campos do device D3D | votos nas instruções alinhadas das funções emparelhadas |
| Que Resolve ligar e onde estão os argumentos (o LTCG faz inline do wrapper e tira argumentos) | segue os usos de cada argumento nas duas listagens |
| Cor de clear por defeito do Resolve | o padrão do código: um float (XDK 6132) ou um vector de 16 bytes (XDK 20353) |
| Superfície da cena para o DLSS | em runtime, dos resolves de um frame (`rexnative scene`) |
| Comportamento do motor (segundo device, valores por defeito) | escolha tua, no `overrides.json` |

A ferramenta escreve tudo num cabeçalho, `game_layer.h`, e numa lista de hooks,
`d3d_surface.inc`. O renderer não lê mais nada sobre o jogo.

O matching trabalha sobre o C++ que o ReXGlue gera (cada instrução PowerPC fica lá como
comentário), sem desassemblador. Para cada função de um port de referência tenta, por ordem de
confiança: a mesma listagem; a mesma com os números mascarados; a única dentro da biblioteca
D3D; a função que uma já emparelhada chama no mesmo sítio (a ideia dos XREF do XbSymbolDatabase
do Cxbx-Reloaded); a vizinha de uma emparelhada (a biblioteca mantém a ordem dos ficheiros
objecto); a que chama as mesmas funções; uma listagem parecida; e um wrapper público que o
compilador fez desaparecer. A referência escolhe-se pelo `default.xex`: primeiro a mesma versão
da biblioteca D3D, depois LTCG ou não, depois a build do XDK mais próxima; as outras preenchem
falhas e confirmam.

## Estado

| Jogo | D3D do XDK | Com este renderer |
|---|---|---|
| Project Gotham Racing 4 | D3D9 2.0.6132 | a origem do renderer: o jogo todo ([pgr4-native](https://github.com/shaunsingh/pgr4-native)) |
| From Dust | D3D9 2.0.20353 | **compilado do ReXNative**: joga-se (menus, níveis, HUD), como com a cópia feita à mão ([FromDustDeck](https://github.com/gtguerreiro-max/FromDustDeck)) |
| Iron Brigade | D3D9LTCG 2.0.20353 | **compilado do ReXNative**: ecrãs de título, HUD, a intro e o seu vídeo, a primeira missão com o cenário (o hang da GPU vinha de argumentos de draw que o LTCG muda de sítio, agora rearrumados; o vídeo é um bitmap do Scaleform desenhado com DrawIndexedVerticesUP, intercetado desde 2026-11-15, num render target com gama; o cenário estático está em command buffers gravados, reproduzidos desde 2026-11-15); joga-se no Steam Deck com o Proton, aos seus 30 FPS |

From Dust e Iron Brigade já compilam o renderer a partir deste repositório (`rexnative_add()` e
um `game_layer.h` gerado), sem cópia própria: testados num PC, cada um desenha o que a sua cópia
feita à mão desenhava. O PGR4 ainda tem a sua cópia no pgr4-native.

### Validação

A camada de cada port foi refeita sem usar esse port e comparada com a feita à mão
(`python -m rexnative validate <port>`):

| | Hooks | Offsets do device | Lista de hooks (`d3d_surface.inc`) |
|---|---|---|---|
| Iron Brigade a partir de From Dust | 24 de 25 iguais, +1 encontrado | 30 de 30 | 82 de 83, +4 |
| From Dust a partir de PGR4 (outro XDK) | 24 de 25 iguais | 30 de 30 (11 tinham mudado) | 104 de 104, +37 |

As diferenças eram, vistas uma a uma, sobretudo erros das camadas feitas à mão. Iron Brigade
passava 0 no argumento `slice` do Resolve, que o seu Resolve interno lê. A sua lista de hooks
tinha um falso emparelhamento. From Dust lia a cor de clear no endereço de PGR4. A superfície da
cena de From Dust ainda era a de PGR4, por isso o upscaler nunca via a cena. O `slice` e a cor de
clear já estão corrigidos nas fontes dos ports. O hook que a ferramenta não achou em From Dust
foi inlined pelo jogo; o que acrescentou a Iron Brigade é a CreateDevice.

## Como usar

Requisitos: Python 3.10+ (a ferramenta não tem dependências); para o renderer, Windows com o
Visual Studio 2022 (ou as Build Tools) com o C++ e as suas ferramentas Clang, git, CMake e
Ninja, e o NVIDIA DLSS SDK e os headers do DLSS 5 (da NVIDIA). O ReXGlue SDK de que o renderer
precisa (fixo, com os seus patches: [sdk/README.md](sdk/README.md)) e o AMD FidelityFX SDK
v2.3.0 vêm de um só script:

```powershell
scripts\build-sdk.ps1
```

**1. Gerar a camada do jogo.** A pasta `generated/default` do ReXGlue e o `default.xex` do
jogo, contra os ports conhecidos do `ports.json` (copia o `ports.example.json` e põe os teus
caminhos: cada referência precisa das fontes e do código gerado do seu port):

```bash
python -m rexnative adapt --generated caminho/do/jogo/generated/default --xex caminho/do/jogo/default.xex --name meujogo --title "O Meu Jogo" --out results/meujogo
```

O `results/meujogo/report.md` diz o que encontrou, como, com que confiança e o que verificar.
Os relatórios e mensagens saem em inglês; com `--lang pt` (ou `REXNATIVE_LANG=pt`) saem em
português.

**2. Juntar o renderer ao build do jogo:**

```cmake
include(caminho/para/ReXNative/renderer/rexnative.cmake)
rexnative_add(meujogo
    LAYER_DIR       ${CMAKE_CURRENT_SOURCE_DIR}/results/meujogo
    REXGLUE_SDK_SRC caminho/para/rexglue-sdk
    FIDELITYFX_SDK  caminho/para/FidelityFX-SDK
    DLSS_SDK        caminho/para/DLSS
    DLSS_NR         caminho/para/dlss_nr)
```

e na app do jogo chamar `rexnative::hud::SetConfigPath`, `Initialize` e `Shutdown`
(`renderer/src/hud/hud.h`). O ReXNative passa a desenhar o jogo por defeito (o
`kDefaultEnabled` da camada); `--rexnative=false` devolve-o ao plugin Xenos do ReXGlue. Todas
as opções são cvars `rexnative_*`; os nomes do PGR4Native (`pgr4_native_*`, `pgr4_d3d_*`)
continuam a funcionar e dizem no log o nome novo.

**3. Medir a cena para o DLSS.** Correr com `--rexnative_resolve_dump=<pasta>`, criar
`<pasta>/dump.request` numa cena 3D, e:

```bash
python -m rexnative scene pasta/f1234-resolves.jsonl
```

Pôr o resultado em `results/meujogo/overrides.json` e correr o `adapt` outra vez.

Outros comandos: `rexnative info <xex>` (título, bibliotecas do XDK), `rexnative validate
<port>`, `rexnative extract` (as camadas dos ports conhecidos), `rexnative neutral` (trazer para
o `renderer/` as alterações do renderer de um port) e `python -m rexnative.checkneutral`
(compilar o `renderer/` com uma camada e as flags do build de um jogo).
[docs/GAME_LAYER.md](docs/GAME_LAYER.md) descreve a camada.

### Depurar um jogo

Arrancar um jogo para cada experiência custa minutos (logos, intro, menus). Em vez disso:

- `scripts\live.ps1 start -Dir <pasta do jogo> -Exe <exe> -Route "<teclas>"` arranca-o uma vez
  numa janela de 1280x720 e leva-o até ao problema; depois cada experiência leva segundos com o
  jogo aberto: `shot` (captura da janela), `debug "<máscara>"` (os interruptores do renderer:
  depth, stencil, cull, blend, alpha test desligados, saltar um intervalo de draws), `snap`
  (render targets depois de certos shaders; `python scripts\snaps.py` passa-os a imagens),
  `drawlog` (os draws de um frame), `trace` (o push buffer do jogo num frame), `log`, `stop`.
  Só uma recompilação obriga a arrancar de novo.
- Com `start -RenderDoc` (a pasta do RenderDoc em `REXNATIVE_RENDERDOC`), `capture` tira uma
  captura RenderDoc do frame seguinte, e o `scripts\rdoc.ps1` interroga-a com o jogo fechado:
  `actions` (cada draw, o número de índices e os targets), `history -X -Y` (pixel history: que
  draws tocaram num píxel e o que falhou), `inputs -Event` (as texturas de um draw em PNG),
  `save`, `buffer -Offset -Size` (bytes da cópia da memória do jogo que o renderer tem, para
  comparar com a do jogo), `mesh`, `disasm`, `cbuffers -Event` (a saída do vertex shader, as
  shaders e as constantes de um draw: comparáveis com uma captura do plugin Xenos).

O cenário estático de Iron Brigade (2026-11-15): a pixel history não mostrou nenhum draw do
G-buffer num edifício cuja profundidade lá estava; a malha só era desenhada nos passos de sombra
e de profundidade, e o trace do push buffer mostrou dois INDIRECT_BUFFER grandes para memória
normal: command buffers gravados (RunCommandBuffer), cujos draws nenhum hook via. Agora são
reproduzidos (ponto 9).

## Próximos passos

Melhorar aos poucos:

1. ~~Compilar From Dust e Iron Brigade a partir do `renderer/` e das camadas geradas, e testar no
   jogo.~~ Feito (2026-11-14).
2. Encontrar a superfície da cena em runtime, para o DLSS funcionar sem a medir.
3. Encontrar o passe HDR antes do tone mapping do jogo, para o DLSS 5.
4. ~~Mudar o nome das opções `pgr4_native_*` e do namespace `pgr4::native` (mantendo os
   antigos).~~ Feito: `rexnative`, `rexnative_*`; o ReXNative fica ligado por defeito.
5. ~~Incluir o conjunto de patches do SDK de que o renderer precisa, fixo, com um script de
   build.~~ Feito (`sdk/`, `scripts/build-sdk.ps1`); falta compilar um jogo de ponta a ponta a
   partir dele numa máquina limpa.
6. Cada port novo passa a referência e facilita o seguinte: jogos com a mesma build do XDK
   emparelham melhor.
7. ~~Os relatórios e mensagens da ferramenta estão em português; acrescentar inglês.~~ Feito:
   inglês por defeito, `--lang pt` para português.
8. Draws com memexport (shaders que escrevem na memória). Correm (2026-11-15) com a memória
   partilhada como UAV (`--rexnative_memexport`), as páginas que escrevem ficam da GPU até o
   CPU lhes escrever, e o que mudaram é escrito de volta na memória do jogo antes de uma
   textura por cima ser lida e no Swap (`--rexnative_memexport_readback`, uma espera pela GPU;
   o snapshot e diff do Xenia Canary). O vídeo preto da intro de Iron Brigade afinal não era
   um deles: o Scaleform desenha a imagem descodificada com DrawIndexedVerticesUP (vértices e
   índices inline, que nenhum hook via; a ferramenta encontra agora o BeginIndexedVertices ao
   lado do DrawVerticesUP), num render target `k_8_8_8_8_GAMMA` que o renderer escrevia linear,
   com metade do brilho (agora na gama da consola, `--rexnative_gamma_targets`; um draw com
   blending sobre um destes alvos usa uma vista sRGB, para misturar em cor linear como a
   consola). Falta: um jogo cuja imagem precise de memexport, e evitar a espera quando nada no
   CPU precisa do resultado.

   Aliasing da EDRAM entre MSAA: uma superfície de profundidade 4x (ou 2x) sobre uma 1x na
   mesma base fica com a profundidade dela (cada amostra é um pixel da 1x), como as partículas
   a meia resolução de Iron Brigade precisam para ficarem escondidas pelo cenário
   (`RenderTargets::AliasDepth`).

   Readback do memexport: um byte que o jogo escreveu depois do draw memexport fica (na consola
   a escrita da GPU veio primeiro); copiado por cima, o carregamento de nível de Iron Brigade
   perdia uma malha para blocos de textura antigos.

   Cópias de memória pela GPU: o motor de Iron Brigade move recursos com uma shader memexport
   que copia blocos de 64 bytes, cerca de 140 vezes por frame. Cada uma precisava de um readback
   à espera da GPU (7 a 17 ms por frame no Steam Deck: os solavancos) e as suas malhas às vezes
   saíam obsoletas (triângulos amarelos enormes). As cópias conhecidas são agora feitas na
   memória do jogo quando ele as pede e nunca chegam à GPU (`--rexnative_gpu_memcpy`); um draw
   memexport espera pelo seu pipeline em vez de ser saltado enquanto compila (o que escreve
   perdia-se de vez).
9. ~~Command buffers gravados (RunCommandBuffer).~~ Feito (2026-11-15). Iron Brigade grava os
   draws de G-buffer do cenário estático num segundo device e corre-os em cada frame com pacotes
   INDIRECT_BUFFER, que nenhum hook via, por isso esse cenário ficava com os dados dos passos
   anteriores (amarelo). O RunCommandBuffer é agora intercetado (overrides.json
   `functions.RunCommandBuffer`), e `renderer/src/native/cmd_replay.cpp` lê a lista de blocos do
   objeto e percorre o seu PM4: escritas de registos, constantes, carregamento de shaders,
   predicação, e os pacotes DRAW_INDX como draws nativos, com o estado que a chamada herda e
   mantém igual ao da GPU. Cada buffer é verificado por inteiro antes de se aplicar o que quer que
   seja. Na primeira missão de Iron Brigade são 2 buffers e 343 draws por frame, cerca de 0,5 ms
   de descodificação. `--rexnative_cmdbuf` 0 desligado, 1 contar e registar, 2 reproduzir (o
   `kDefaultCmdBuf` do layer); o `--rexnative_d3d_control` aceita `cmdbuf 0|1|2`, `cmdbuf log N`
   e `cmdbuf dump [etiqueta]` (cada pacote dos dois buffers seguintes para a pasta do trace); os
   bits 4096 e 8192 do draw-debug.txt saltam os draws reproduzidos ou todos os outros.
10. ~~Pipelines compilados de antemão.~~ Feito (2026-11-15). Os pipelines que um jogo cria
    ficam guardados (`<cache_root>\rexnative\pipelines.bin`, mais uma biblioteca de pipelines
    D3D12) e são todos compilados outra vez ao arrancar, em segundo plano, os usados mais
    recentemente primeiro (no máximo 4096); nenhum é esquecido
    (`--rexnative_pipeline_prewarm_sessions` e `--rexnative_pipeline_keep_sessions` limitam-nos
    às últimas N sessões). Só um pipeline que o jogo nunca criou ainda segura um frame enquanto
    compila (50 a 170 ms no Steam Deck). O uso de um pipeline guardado era marcado noutro registo
    (os registos eram ordenados à parte dos seus hashes), por isso pipelines em uso podiam ser
    esquecidos.
11. Outros jogos, de ponta a ponta (começado a 2026-11-15). `python -m rexnative project
    <work>/<jogo> --build` faz de um jogo que o `prepare` preparou um projecto ReXGlue desenhado
    pelo ReXNative (CMakeLists, a app com o HUD de definições, um ficheiro de configuração),
    compila-o e monta `run/` (o executável, as DLLs, `game` como ligação aos dados); `--discover N`
    corre-o e faz de cada função a que o jogo chega sem que o codegen a tenha feito (a entrada de
    uma thread, uma chamada indireta) uma pista do codegen, e compila outra vez. Os nomes dos
    projectos são uma palavra só: a cola CMake do ReXGlue junta as palavras de um nome
    (`jetsetradio_pch.h`) onde as suas fontes as mantêm separadas. O comando do HUD de definições
    vem do XInputGetState do jogo (o wrapper de três instruções do XDK, encontrado pela forma),
    intercetado em todos os jogos, por isso o Back + Start funciona em todos.

    O que um jogo novo precisa, feito pela ferramenta (`project --find-functions`,
    rexnative/functions.py): os inícios de funções que o codegen não encontrou, procurados de
    antemão na imagem carregada (`--rexnative_dump_image`): o que é apontado, construído com
    lis + addi, com prólogo, ou deixado entre funções depois de código que não continua (Super
    Meat Boy 3523, Jet Set Radio 4280; as rotinas do XDK que guardam e repõem registos nunca
    viram funções, o codegen trata as chamadas a elas); tabelas de saltos que o codegen leu
    curtas, pela verificação do limite (`cmplwi`, `bgt`, `lis`/`addi`, `lwzx`, `bctr`), dadas
    inteiras como `[[switch_tables]]` (o Double Dragon Neon parava nos casos 16 a 143); gotos
    para labels de outras funções feitos funções (Minecraft); imports que o runtime não tem
    feitos stubs (a câmara Vision, `XUsbcam*`); uma função por endereço no layer (Perfect Dark,
    Bionic Commando Rearmed). A app escreve `crash.txt` (pilha do anfitrião; o
    `build/<nome>.map` dá os nomes das funções do jogo). Primeiros resultados no PC
    (2026-11-15): Dungeons & Dragons: Chronicles of Mystara joga-se (menus, escolha de capítulo,
    o primeiro capítulo, 60 FPS); Double Dragon Neon chega ao ecrã de título; Castlevania:
    Symphony of the Night à moldura do menu; Jet Set Radio pára no descodificador de JPEG
    (libjpeg) logo depois de arrancar, também com o renderer original.

Não há neste repositório ficheiros dos jogos, código gerado nem dados dos jogos: precisas da tua
cópia de cada jogo e do seu projecto ReXGlue.

## Licença e créditos

MIT ([LICENSE](LICENSE)). O renderer é do pgr4-native (MIT), com código portado do ReXGlue SDK e
do Xenia (BSD-3-Clause); ver [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). O NVIDIA DLSS e o
AMD FidelityFX vêm dos seus SDKs, com as suas licenças, e não estão neste repositório. Xbox 360 é
uma marca da Microsoft; este projecto não tem ligação à Microsoft, à NVIDIA, à AMD nem às
editoras dos jogos.
