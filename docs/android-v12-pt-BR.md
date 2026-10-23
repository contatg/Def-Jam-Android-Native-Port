# Android v12 — ENTER/BACK e framegen experimental

Aplicação cumulativa sobre v9, v10 ou v11. Preserva launcher, editor, D-pad agrupado, controles personalizados
da luta, menu do port por toque, FPS30..165, CPU selecionável e preparação paralela de vértices do v11.

## Controles

O pad Xbox completo deixou de ser criado nos menus. As ações ficam na barra: ENTER (a tecla Enter/START
que já iniciava o jogo) e BACK (Backspace/BACK). Na luta, ENTER volta a se chamar START e BACK fica oculto;
os controles personalizados seguem iguais. CONTROLES, PAD:AUTO/MENU/LUTA e MENU continuam como utilidades.
Não foi acrescentado toque nos itens dos menus originais do jogo nem um novo direcional de menu.

## Framegen

No launcher há **Framegen2x • experimental • reiniciar**, desligado por padrão. Comece em1x e cap120,
com GPU de sobra. Reinicie para aplicar. A opção seleciona FIFO para manter a ordem de apresentação das
imagens intermediárias e reais. Desativar e reiniciar restaura o modo de apresentação anterior.

Implementação própria inspirada no princípio do TK3, não uma cópia do renderizador de PS2: guarda as
constantes dos programas de vértices de draws com assinaturas compatíveis entre frames, interpola em50%,
reexecuta a lista de comandos GPU, mostra essa imagem e restaura os valores atuais exatos antes de
reexecutar e mostrar a imagem real. Uploads de texturas rodam só uma vez; buffers/descritores/recursos
ficam vivos até as duas execuções terminarem. Cada alteração da UBO ocorre após a fence correspondente.

A lógica, entrada e número de ticks do jogo continuam os originais; HUD/efeitos transformados pela CPU
ficam no estado atual. O prototype não inclui os ajustes específicos de câmera, cenário, paredes e efeitos
do TK3. Interpolar matrizes pode deformar elementos; objetos sem par ficam atuais. Não é optical flow,
IA nem mistura de duas imagens prontas. Pode adicionar custo de GPU e latência ou reduzir FPS reais.

Não gera intermediárias em menus, overlay do port, cenas sem movimento correspondente, mudanças grandes
de constantes, maioria dos draws sem correspondência, flush/leitura parcial, captura ou falta de folga na
GPU. Exige limpar cor/profundidade antes de desenhar (stencil também, quando usado). O cap deve ser>=60
e os FPS reais observados devem caber na metade do menor valor entre cap e frequência conhecida da tela.
Se essas condições não forem atendidas, usa o frame real. É normal a opção estar ligada e mostrar0FG.

O HUD mostra `FPS reais + FG adicionais`, sem somar os gerados ao desempenho real. Logs `[FRAMEGEN]`
mostram FPS extras e, a cada120frames reais, correspondências, movimento, clears, custo GPU e limite.
Não há medição de ganho de FPS, correção visual ou latência em aparelho neste ambiente.

## Performance e referências

Foi implementado reaproveitamento de constantes de pixel shader idênticas dentro do mesmo buffer de
comandos. Uma mudança do estado ou do número de frame/alocação invalida o reaproveitamento. O teste
confirma que100pedidos idênticos usam1alocação, e alterações ou novo frame pedem uma nova. Isso não é
um benchmark de FPS. O cache de shaders/pipelines já existia; foi preservado, sem acrescentar outro cache.
Veja `docs/research/android-renderer-references-2026-10-22.md` para a análise dos dois projetos.

## Validação e teste necessário

8fixtures host passaram: entrada/afinidade/pacing/toque, vértices paralelos, histórico/interpolação,
orquestração de replay com chamadas Vulkan simuladas e reutilização real de constantes PS. Fixtures de
memória passaram com ASan/UBSan, exceto o detector de vazamentos desativado pela restrição de `/proc` do
ambiente. O Vulkan C inteiro passou por verificação de sintaxe em Android/Linux com headers oficiais;
8classes Java foram analisadas somente quanto à sintaxe. Não houve build de APK, submissão Vulkan real,
teste no celular nem regressão de gameplay. As simulações não provam apresentação correta num driver.

Faça60s da mesma luta com FG desligado e60s ligado, mesma resolução/cap/CPU. Guarde cada log antes de
abrir de novo. Compare FPS reais e artefatos, não somente a soma. Desligue FG e reinicie se piorar.

Aplicar: `Aplicar-v12.ps1 -ProjectRoot <projeto> -Build`. Validação de hashes antes de escrever; backup em
`logs/v12-backups`. Atualiza timestamps para recompilar fontes nativas, inclusive as do erro de link anterior.
