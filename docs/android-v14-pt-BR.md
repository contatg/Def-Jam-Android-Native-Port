# v14 — renderização e coleta para FPS real

Aplica sobre v12 ou v13, incluindo a recuperação do renderizador, MENU embaixo, A/B e D-pad e as opções
fixas30/60/90/120/144/165. Sem framegen e sem mexer no timer de simulação.

## Otimização com Black como referência

Black (`RanggaCasper/Black-Android`, snapshot f3754d6985a8c35204e7df071546e586b10f1806) separa trabalho
de renderização e envia mudanças do estado. O v13 já reaproveita descriptor sets por frame. O v14 evita
comandos idênticos de binding de descritores/offsets, blend e stencil. Zera a validade ao abrir um novo
buffer ou render pass, para não reutilizar estado inválido após gamma/overlay/flush. Não troca o runtime
inteiro, não cria frames falsos e não aumenta a taxa da lógica. `[VKSTATE]` mostra comandos evitados.
Isso reduz comandos nos casos repetidos testados; ganho de FPS no celular ainda não foi medido.

## O que falta para120/144/165FPS reais

Não estão desbloqueados neste pacote. O ZIP original não trouxe `src/recomp/gen` nem os corpos de
`sub_00011CD0`, `sub_000D75E0` e funções do produtor de passos. O estudo anterior de simulação identifica
passos fixos60Hz e0..4passos por render; o combate ignora o dt em segundos. Mudar o teto de apresentação,
alterar só o flip timer ou copiar endereços do Black não demonstra jogo normal em120FPS.

O Black altera tempo dos timers, menus, animações e física em pontos específicos daquele jogo. É preciso
ler e adaptar os pontos equivalentes do Def Jam antes de dizer que120FPS reais estão funcionando.

O instalador executa `scripts/collect-high-fps.py` e cria em Downloads `DefJam-FPS-Fontes-<data>.zip`.
Envie esse ZIP aqui: contém funções selecionadas de tempo/main/input/render, dependências imediatas e
um manifesto, até80funções/8MiB. Não inclui ISO/XBE/APK. É diagnóstico privado; não publique os fontes
recompilados do jogo. Não é necessário enviar novamente o projeto inteiro.

## Build e validação

Gradle agora usa `--no-daemon --max-workers=1`, heap2GiB e metaspace512MiB. Não limpa objetos compilados
nem força regeneração. Reduz concorrência após o crash da JVM, mas não diagnostica a causa sem hs_err.

7fixtures host:4 anteriores de entrada/afinidade/pacing/Dpad/ImGui, cache de descritores, novo estado
dinâmico e coletor de fontes. Vulkan completo passou sintaxe Android/Linux com headers oficiais.
Não houve build de APK, gameplay, benchmark no telefone ou desbloqueio real de120..165FPS.

Para comparar performance, mantenha mesma luta/resolução/CPU e capture60s. Observe FPS reais e `[PERF]`,
não apenas quantos comandos `[VKSTATE]` evitou. Instalar APK após build bem-sucedido com ADB.
