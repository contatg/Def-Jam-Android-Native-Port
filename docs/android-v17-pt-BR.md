# v17 — otimizações do caminho de renderização

Base: **v16**. APK esperado depois de compilar localmente: **0.5.3-v17-render**, versionCode 17.
Não há APK pronto neste ZIP. Não foi medido FPS em aparelho nesta etapa.

## Mudanças implementadas

1. **Chave de shader separada das cores.** O v16 colocava os valores de c0/c1 e das constantes
   finais na chave de busca do combiner. Alterar uma cor fazia procurar outra configuração e
   podia gerar o mesmo HLSL de novo, mesmo quando o cache posterior reutilizava o módulo.
   Agora a chave contém a configuração do programa, mantendo as cores no buffer de uniformes.
   Estágios inativos não poluem a chave. Um acesso recente evita repetir o hash de todo o estado.
   Mudanças no programa ativo ou na forma de amostrar texturas continuam escolhendo outro shader.
2. **Uniformes iguais reutilizam a mesma faixa.** Constantes PS e de tela idênticas deixam de
   ocupar outra faixa do buffer a cada draw. Isso também permite que a eliminação de binds
   redundantes do v14 funcione em mais draws. Cada mudança de valor exige nova cópia; a comparação
   é por bytes, preservando zero com sinal e payloads NaN. Cada início de command buffer invalida
   a reutilização, inclusive quando há overflow ou troca de contexto. Não reutiliza dados em voo.
3. **Preparação dos vértices float por lote.** Identifica o formato e valida o intervalo de índices
   uma vez por faixa de trabalho. Os atributos float válidos são copiados em bloco com os valores
   padrão corretos para as componentes ausentes. Outros formatos, spans suspeitos e buffers
   truncados continuam no decodificador anterior. Não remove validação de endereços nem muda a
   lista de índices, ordem de draws, precisão ou número de objetos desenhados.

Não muda resolução, qualidade, simulação, limite de FPS ou escalonamento guest. Preserva a fila
Vulkan de 2 contextos do v16, fences, semáforos, aposentadoria de texturas, capturas e retorno de
superfície. Nenhum arquivo Java de launcher/controles e nenhum arquivo de save é substituído.

## O que foi aproveitado do Black

Fontes consultadas novamente em 22/10/2026, no repositório
https://github.com/RanggaCasper/Black-Android:

- native/vulkan.cpp, blob **a1cc885f64fd883dfbca26760aac7b0e60561d62**: BuildPipelineKey,
  Draw/ClassifyShaders, uploads, descritores e ciclo de vida dos frames.
- src/d3d/render_thread.cpp, blob **b7cd0f82c5d123359e2b7d99e58bc9090b063027**: StateRanges,
  Same/SendState/Delta e classificação dos dados enviados ao renderizador.

| Mecanismo | Situação no Def Jam |
| --- | --- |
| Thread de renderização | Já existe como executor do push buffer |
| Comandos em voo com recursos protegidos | Fila de 2 contextos do v16, mantida |
| Buffers de upload mapeados e caches de shaders/pipelines | Já existentes, mantidos |
| Descritores e estado sem reenvio redundante | v14 existente; v17 amplia a reutilização dos uniformes |
| Separar programa do shader e constantes por draw | Adaptado em C no v17 |
| Classificar formato de vértices antes do loop | Adaptado no v17 para streams float válidos |
| Streams com formatos nativos lidos diretamente pela GPU | Não portado; o Def Jam ainda usa seus atributos compactados float4 |
| Alocador de memória em blocos, dynamic rendering e timeline semaphores do Black | Não portados; continuam os caminhos do Def Jam |

**Este pacote não contém o renderizador inteiro do Black.** Ele usa outro runtime C++, estruturas
de estado/recursos e tradução de shaders. Substituir seu vulkan.cpp no Def Jam não fornece essas
dependências. As mudanças acima são adaptações do princípio de separar estado estável/dinâmico
e preparar o formato antes de processar os vértices. A reutilização de uniformes é uma extensão
própria para este backend: o Constants() do Black consultado ainda faz upload por chamada.
Não transplanta código comercial do jogo Black nem suas funções recompiladas.

## Aplicar e compilar

Extraia em Downloads e rode no PowerShell:

```powershell
& {
    $ErrorActionPreference = "Stop"
    Set-ExecutionPolicy -Scope Process Bypass -Force
    $env:Path = "C:\dev\gradle-8.7\bin;$env:Path"
    $project = "C:\Users\raylander\Downloads\DefJamFFNY-Android-local-bringup (1)"
    & "$env:USERPROFILE\Downloads\DefJam-Android-v17\Aplicar-v17.ps1" -ProjectRoot $project -DataDir "C:\Games\DefJamFFNY-data" -Variant Release -Build
    & "$env:LOCALAPPDATA\Android\Sdk\platform-tools\adb.exe" install -r "$project\android\app\build\outputs\apk\release\app-release.apk"
    if ($LASTEXITCODE) { throw "ADB nao instalou o APK." }
}
```

O instalador valida hashes antes de gravar e faz backup dos arquivos substituídos em
logs/v17-backups. Se os fontes não corresponderem ao v16, interrompe sem sobrescrever.
Build precisa terminar com sucesso antes de instalar; não desinstale nem limpe os dados.

## Comparação e reversão dos caminhos novos

As otimizações vêm ligadas. O início do log mostra `render fast 1 (v17...)`. Compare a mesma luta
com a mesma resolução, driver, async, teto e quantidade de núcleos; comece em **1 núcleo**, que já
chega à luta no log disponível. `[VKFAST]` mostra cópias/reutilizações de uniformes e buscas/gerações
de combiners por janela. `[D3D]` continua mostrando apresentações reais. Os contadores novos não
são uma medição de FPS ganho nem do tempo físico da GPU.
`async Vulkan queue 1 (v16)` continua identificando o mecanismo de fila preservado; a versão
do APK é v17 e a linha render fast identifica as otimizações novas.

Para comparar com os caminhos anteriores, com o jogo fechado:

```powershell
$adb = "$env:LOCALAPPDATA\Android\Sdk\platform-tools\adb.exe"
& $adb shell touch /storage/emulated/0/DefJamFFNY/render-v16.txt
```

Abra o jogo novamente. O log deve mostrar `render fast 0`. A fila async do v16 continua igual.
Para voltar às otimizações, feche o jogo, remova **somente esse marcador** e reabra:

```powershell
& $adb shell rm -f /storage/emulated/0/DefJamFFNY/render-v16.txt
```

Copie cada rodada diretamente do telefone antes de abrir a seguinte:

```powershell
& $adb pull /storage/emulated/0/DefJamFFNY/defjam-native.log "$env:USERPROFILE\Downloads\defjam-v17-luta.log"
```

Troque o nome local entre as rodadas para preservar ambas. Não copie o log de C:\Games.

## Loading multicore e tamanho dos arquivos

O travamento com 2..8 CPUs continua sem captura própria. Este patch de renderização **não é uma
correção comprovada desse loading**. A afinidade guest não foi alterada e RECOMP_GUEST_LOCK não
foi ativado. A pasta Coleta-v16 contém o coletor direto do telefone; veja seu LEIA-ME. Pode ser
usado no v16 instalado antes de compilar/instalar esta atualização. Não precisa outro APK.

As imagens Partition*.img da pasta save têm os tamanhos lógicos das partições Xbox. O código
usa ftruncate, que pode criar regiões sem blocos físicos alocados. A captura do ZArchiver não
mostra, sozinha, quanto foi realmente ocupado. O coletor inclui ls -l e du -k para comparar.
Nenhum desses arquivos, UserData ou TitleData é apagado ou modificado pelo instalador/coletor.

## Validação real

Os fixtures host exercitam código de produção: mesma geração HLSL apesar de 200 variações de
cores (200 chamadas no caminho antigo, 1 no novo), mudança estrutural/cache cheio, 100 draws
idênticos com 200 cópias de uniformes reduzidas a 2, atualização de valores e invalidação por
contexto. A preparação dos vértices é comparada byte a byte nos dois caminhos, incluindo todos
os formatos anteriores, float1..4, valores padrão, intervalos inválidos e falha de workers.
ASan/UBSan usados. A fila Vulkan continua exercitada contra chamadas GPU simuladas, inclusive
ownership de buffers em voo, descarte, readback, overflow, falhas e reset dos caches novos.

Também verificadas a sintaxe do backend completo nos caminhos Android/Linux e a coleta shell
em fixtures host. PowerShell não foi executado no Windows aqui. Não há Android SDK/NDK, código
guest gerado/dados do jogo ou aparelho conectado neste ambiente. Não foi gerado APK, executado
gameplay, validado retorno do segundo plano ou medido ganho de FPS. Abertura/loading/luta/resume
em 1/2/4/8 CPUs seguem pendentes de teste no telefone. Compilação não substitui esses testes.
