# v13 — correção após o log do aparelho

Menu do jogo: A e B embaixo à direita; D-pad à esquerda com teclas reais do direcional. A=K e B=L,
os padrões do port. Barra de utilidades, incluindo MENU(F1), embaixo ao centro. Controles de luta e
editor preservados. O editor libera os botões de menu; pausa/perda de foco também liberam teclas.

O log real do v12 termina com7–12FPS. [PERF] mediu101.06ms/frame:6.11ms gather e53.78ms draw-submit.
O framegen capturava cerca de563draws/frame, mas não gerava quadros. A implementação acrescentava
buffers únicos e cópias de constantes mesmo rejeitando a interpolação. Isso é uma regressão minha.
A correção restaura integralmente o Vulkan anterior ao framegen, retirando esse trabalho e o FIFO
forçado. Remove a opção FG; desliga FG e vértices paralelos salvos uma vez ao abrir o launcher.
É recuperação de regressão, sem promessa de ganho em aparelho ainda não testado.

FPS: opções fixas30/60/90/120/144/165. São tetos de apresentação, não certificado de FPS real.
Desbloqueio real de120..165 mantendo a velocidade normal NÃO foi implementado: flip_pace em
nv2a_pb_exec.c usa60Hz para o relógio do jogo. O próprio código documenta luta1.3x acelerada ao
remover essa espera. Não foi aumentado o relógio guest cegamente. Precisa separar simulação/render.

O log também mede3.2..3.7núcleos equivalentes do processo com5permitidos; inclui áudio/render.
Portanto não prova que todo o processo esteja usando só1núcleo. Não há benchmark pós-correção.

Verificações host:8Java parse de sintaxe; Vulkan Android sintaxe com headers oficiais;4fixtures nativos,
Dpad e ImGui real. Sem compilação Android, teste de gameplay ou confirmação visual no celular.
Aplicar sobre v12. Backup em logs/v13-backups, hashes validados antes das cópias. Instale com ADB,
abra o launcher para a migração, mantenha resolução1x e compare a mesma luta60s com novo log.

## Técnica adaptada do Black

Os dois ports já usam Vulkan. Em Black/native/vulkan.cpp, cada frame mantém um mapa de descriptor sets.
Foi adaptada essa técnica: cache por frame de image views/samplers, com comparação completa da chave,
reutilização quando materiais alternam, limpeza quando o descriptor pool é reiniciado e fallback normal
se a tabela estiver cheia. Não foi substituído todo o backend/API de comandos e recursos por outro runtime.
Teste host passou em reutilização repetida, colisões, saturação e limpeza. Vulkan Android/Linux passou
na análise de sintaxe. Não há benchmark no celular; redução de alocações não garante ganho de FPS.
