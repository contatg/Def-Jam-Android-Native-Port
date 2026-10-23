# v15 — ações corretas e menos trabalho de CPU

Aplicar sobre v13 ou v14, com backup e verificação dos arquivos antes de qualquer cópia.

## Botões na luta

| Botão desenhado | Entrada corrigida |
| --- | --- |
| SOCO | Y / tecla I |
| CHUTE | X / tecla J |
| AGARRAR | A / tecla K |

Corrige o ciclo relatado: SOCO fazia CHUTE, CHUTE fazia AGARRAR, AGARRAR fazia SOCO.
Na primeira abertura com tamanho de tela válido, muda somente essas três associações no layout
salvo. Preserva posições, tamanho, visibilidade, D-pad agrupado e demais botões. A marca fight_keys_v15
impede que alterações posteriores feitas no editor sejam sobrescritas. Padrão usa o mapa corrigido.
Não precisa limpar dados nem remontar o controle. Menu com A/B/Dpad e MENU inferior preservados.

## Desempenho

O log novo confirma v14, CPU5, vértices paralelos5, teto165, resolução854x480 e Turnip/Adreno610.
Na luta aparecem aproximadamente20–22FPS, com queda tardia até6,5FPS. [PERF] mede5,55–5,77ms de
coleta e5,47–5,68ms de draw-submit em46,95–48,51ms de frame. A GPU fence fica perto de12ms.
O log não permite atribuir a queda tardia a aquecimento, núcleos ou a uma mudança específica.

Alterações deste pacote:

- A lista de atributos ativos é montada uma vez por faixa de vértices. Evita examinar os16 slots
  para cada vértice. O decoder, a ordem dos atributos, os índices e a saída continuam iguais.
- Nas esperas pelo executor/interrupção GPU, após250µs o Android pede pausas de100µs entre consultas.
  Mantém SwitchToThread, condições originais e limites de50/100ms. Não ignora fence nem sinaliza
  conclusão antecipada. O escalonador pode retomar depois dos100µs pedidos; o efeito no telefone
  precisa ser comparado. Objetivo: reduzir CPU gasta esperando e concorrência com o executor.

As escolhas salvas de FPS, CPU, resolução e paralelismo são mantidas. As opções30–165 continuam
sendo tetos; este pacote não desbloqueia120/144/165FPS de simulação nem altera a velocidade da luta.

## Verificação feita

Cinco fixtures host passaram: afinidade real1..8, pacing/contexto de entrada, geometria Dpad,
saída do gather/pool sob ASan/UBSan e os dois loops de espera reais com relógio simulado.
O teste de espera verifica prontidão, espera curta sem nanosleep, espera longa, EINTR, timeout,
flags desligadas, IRQL e escape original de1segundo. Nenhuma conclusão de fence é fabricada.
Oito arquivos Java passaram no parser17 (sintaxe, sem resolver Android SDK). kernel_bridge.c
passou sintaxe host pelo caminho Android, com warnings preexistentes.

Microbenchmark x86 isolado,3 atributos float4/512 vértices/10mil lotes:214–257ms antes e140–150ms
depois, em3 rodadas. Mede somente aquele laço no PC; não equivale a um ganho de FPS no Android.
Não foi compilado APK nem testado gameplay no aparelho aqui. Não existe ganho de FPS confirmado.

## Aplicar e comparar

Execute Aplicar-v15.ps1 -ProjectRoot <raiz> -Build. O instalador valida tudo e cria backup em
logs/v15-backups antes de copiar. Um erro de hash interrompe a aplicação sem gravar arquivos.
O coletor de fontes de tempo do v14 é mantido para a investigação separada de FPS real acima de60.
Após o build terminar, instale o APK com adb install -r. Não desinstale o app nem limpe seus dados.
Teste as3 ações e capture60segundos na mesma luta, mantendo os mesmos ajustes e condições.
Envie defjam-native.log novo para comparar FPS real e CPU com v14. Se houver falha de build,
não instale APK antigo; envie o primeiro erro de compilação e o trecho final do Gradle.
