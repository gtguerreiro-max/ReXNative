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
  comando) que grava no ficheiro de configuração do jogo;
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
| From Dust | D3D9 2.0.20353 | joga-se, 60 FPS na Steam Deck, HUD ([FromDustDeck](https://github.com/gtguerreiro-max/FromDustDeck)) |
| Iron Brigade | D3D9LTCG 2.0.20353 | primeiros ecrãs desenhados; pára no primeiro loading (em investigação) |

Esses ports ainda têm a sua cópia do renderer. O `renderer/` deste repositório é esse código
tornado neutro: **compila** (`clang-cl /Zs`) com as camadas dos três jogos, mas ainda nenhum jogo
corre a partir dele. É o próximo passo.

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
clang-cl do Visual Studio 2022, um ReXGlue SDK com o conjunto de patches dos ports (os
`patches/` do FromDustDeck), o AMD FidelityFX SDK v2.3.0, o NVIDIA DLSS SDK e os headers do
DLSS 5.

**1. Gerar a camada do jogo.** A pasta `generated/default` do ReXGlue e o `default.xex` do
jogo, contra os ports conhecidos do `ports.json` (copia o `ports.example.json` e põe os teus
caminhos: cada referência precisa das fontes e do código gerado do seu port):

```bash
python -m rexnative adapt --generated caminho/do/jogo/generated/default --xex caminho/do/jogo/default.xex --name meujogo --title "O Meu Jogo" --out results/meujogo
```

O `results/meujogo/report.md` diz o que encontrou, como, com que confiança e o que verificar.

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
(`renderer/src/hud/hud.h`). Correr com `--pgr4_native_primary=true`.

**3. Medir a cena para o DLSS.** Correr com `--pgr4_native_resolve_dump=<pasta>`, criar
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

## Próximos passos

Melhorar aos poucos:

1. Compilar From Dust e Iron Brigade a partir do `renderer/` e das camadas geradas, e testar no jogo.
2. Encontrar a superfície da cena em runtime, para o DLSS funcionar sem a medir.
3. Encontrar o passe HDR antes do tone mapping do jogo, para o DLSS 5.
4. Mudar o nome das opções `pgr4_native_*` e do namespace `pgr4::native` (mantendo os antigos).
5. Incluir o conjunto de patches do SDK de que o renderer precisa, fixo, com um script de build.
6. Cada port novo passa a referência e facilita o seguinte: jogos com a mesma build do XDK
   emparelham melhor.
7. Os relatórios e mensagens da ferramenta estão em português; acrescentar inglês.

Não há neste repositório ficheiros dos jogos, código gerado nem dados dos jogos: precisas da tua
cópia de cada jogo e do seu projecto ReXGlue.

## Licença e créditos

MIT ([LICENSE](LICENSE)). O renderer é do pgr4-native (MIT), com código portado do ReXGlue SDK e
do Xenia (BSD-3-Clause); ver [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). O NVIDIA DLSS e o
AMD FidelityFX vêm dos seus SDKs, com as suas licenças, e não estão neste repositório. Xbox 360 é
uma marca da Microsoft; este projecto não tem ligação à Microsoft, à NVIDIA, à AMD nem às
editoras dos jogos.
