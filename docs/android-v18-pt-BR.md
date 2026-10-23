# v18 — controles de menu e CPUs nativas independentes

Patch de fontes para **v16/v17**, com APK esperado **0.5.4-v18-native**, versionCode 18.
O ZIP não contém APK. Há seis fixtures host aprovados; ainda não há teste deste v18 no celular.

## Controles na tela enviada

**Choose Style / Kickboxing** passa a usar o conjunto de menu: direcional junto, A para confirmar
e B para voltar. As chamadas `Game.ShowFightingMovie` e `Game.ChooseFightingStyle`, vistas no log,
mantêm esse modo durante a escolha e sua prévia. `Game.EnterTutorialMode` devolve os controles
de luta. As páginas de criação do personagem também são reconhecidas. A identificação pelo
módulo sozinho falhava porque essas páginas existem dentro do módulo Story/jogo.

O menu touch do port continua tendo prioridade. START, MENU e editor permanecem na barra
inferior. PAD: AUTO/MENU/LUTA continua disponível para uma página ainda não reconhecida.
Layouts salvos e funções de luta são preservados: SOCO=Y, CHUTE=X, AGARRAR=A, DEFENDER=RT,
CORRER=B, ESPECIAL=LT. Não há substituição dos arquivos de arte ou dos controles personalizados.

## O que muda no uso das CPUs

| Opção do launcher | O que controla |
| --- | --- |
| Núcleos guest, 1 a 8 | Máscara das threads que executam o Xbox recompilado e seus callbacks de interrupção. A política anterior foi mantida. |
| CPUs nativas, Auto ou 1 a 8 | Máscara do executor de renderização, relógio nativo, callback de áudio e workers de vértices. Auto permite as CPUs disponíveis no início do processo, respeitando o Android. |
| Preparação paralela dos vértices | Permite até 8 participantes nos lotes grandes, de acordo com o orçamento nativo. Não depende mais do número guest. |

Antes de restringir o guest, o port agora guarda sua máscara original. Threads nativas conhecidas
recuperam essa máscara ou o subconjunto escolhido, mesmo quando nasceram de uma thread guest
restrita. A função que fixa o guest não foi ampliada nem substituída. Um callback nativo executado
pela própria thread guest conserva a máscara guest; não ganha CPUs por acidente.

Na primeira abertura do launcher v18, o orçamento nativo é inicializado em **Auto** e a preparação
paralela é ligada. Isso desfaz apenas a desativação herdada da recuperação v13. As escolhas
posteriores são respeitadas. Guest, driver, async, FPS e saves não são redefinidos por essa migração.

O pool de vértices cria workers persistentes conforme necessário, aguarda com condition variables
e acorda somente os participantes do lote atual. Pode crescer depois de lotes menores. Lotes
menores que 2048 vértices ficam seriais; cada participante recebe pelo menos cerca de 1024 vértices.
Cada worker escreve uma faixa distinta e o executor aguarda a conclusão antes de usar o resultado.
Não há execução de código guest nem chamadas Vulkan dentro desses workers.

**Permitir 8 CPUs não significa manter 8 ocupadas.** O paralelismo depende das tarefas e do tamanho
dos lotes. A escolha guest de 1 a 8 continua permitindo execução guest simultânea, com os riscos de
compatibilidade já conhecidos; não transforma um trabalho serial em oito trabalhos. O v18 não é
uma correção comprovada de todos os loadings com 2 a 8 núcleos.

O launcher envia um snapshot das opções por Intent para o processo `:game`. DefJamActivity usa
esse snapshot para argumentos e HUD; SharedPreferences passa a ser fallback para abertura direta.
Isso evita depender do cache de preferências de outro processo para CPU, FPS, async e driver.

## Comparação com Black

Fontes públicas consultadas em 22/10/2026:

- [Black: render_thread.cpp](https://github.com/RanggaCasper/Black-Android/blob/0eb0fe3f2708a2403fc1156a91563f337c5bc409/src/d3d/render_thread.cpp), blob `b7cd0f82c5d123359e2b7d99e58bc9090b063027`: thread de renderização própria, lotes e espera por condição.
- [Black: game_host.cpp](https://github.com/RanggaCasper/Black-Android/blob/0eb0fe3f2708a2403fc1156a91563f337c5bc409/native/game_host.cpp), blob `d867bc1962a6d0958880e1aab5a213b90f3c7636`: backend com execução em thread e pausa.
- [Black: mixer.cpp](https://github.com/RanggaCasper/Black-Android/blob/0eb0fe3f2708a2403fc1156a91563f337c5bc409/src/sound/mixer.cpp), blob `94416b0bb47bf0c1fb4b29a0de06a4063fbd9e1e`: thread host `dsound`.
- [SDK: threads.cpp](https://github.com/TGP482/XboxXBERecomp/blob/1bb872b181c40de83ca2f69d9cd45b442a362d1d/runtime/kernel/threads.cpp), blob `56fadb8ef5748a87338363b0d72bf89056b7d11f`: pthreads host/guest e contexto guest por thread.
- [Black: vulkan.cpp](https://github.com/RanggaCasper/Black-Android/blob/0eb0fe3f2708a2403fc1156a91563f337c5bc409/native/vulkan.cpp), blob `a1cc885f64fd883dfbca26760aac7b0e60561d62`: referência para a fila e os caches das etapas anteriores.

Nos arquivos de threads/renderização consultados não há uma opção mágica que distribua todo o
jogo entre oito CPUs. Há separação de tarefas e threads que o escalonador pode executar em
paralelo. `SDL_GetNumLogicalCPUCores` mostra a quantidade de CPUs do aparelho; esse número não
mede quantas estão trabalhando. O SDK em main também não comprova qual commit está em um APK
Black já publicado.

O v18 adapta esse princípio ao runtime C existente: separação do orçamento nativo, masks herdadas
corrigidas para papéis conhecidos e workers bloqueados quando ociosos. Mantém a fila Vulkan
assíncrona de dois contextos do v16 e as otimizações v17 de shaders, uniformes e streams float.
Não transplanta o backend C++ inteiro, seu alocador, streams nativos na GPU, dynamic rendering ou
timeline semaphores. Nenhum código recompilado ou dado comercial do jogo Black entra neste ZIP.

## O que o log recebido demonstra

O log de 5283 linhas começa com **v17, guest request 2, máscara aplicada 2, teto 30 e async ligado**.
Chega a ChooseFightingStyle, EnterTutorialMode e à luta. Portanto essa tentativa com 2 abriu;
não reproduz o loading travado relatado antes. A imagem com CPU 1/1 corresponde a outra escolha.

O mesmo log informa **requested 1 participant** para os vértices. O processo chega a aproximadamente
3,07 núcleos equivalentes, incluindo todas as suas threads. Assim, o port já tinha trabalho em
várias threads, mas a preparação dos vértices ficou serial nessa rodada. Não dá para atribuir
todo o tempo restante a uma thread guest, nem prever ganho de FPS só contando CPUs permitidas.

Um relatório de luta registra 59,28 ms/frame, 7,29 ms de gather e 9,25 ms de draw-submit,
579 batches/frame. O fim mostra 18,3 FPS. Espera baixa por fence em async não mede a duração
física da GPU. O log termina pouco depois de apresentações; não é a interrupção antiga com
cerca de 1900 linhas de kernel sem novos frames. Não há comparação controlada 1 contra 2.

## Aplicar, compilar e instalar

Baixe o ZIP em Downloads e rode o bloco completo no PowerShell:

```powershell
& {
    $ErrorActionPreference = "Stop"
    Set-ExecutionPolicy -Scope Process Bypass -Force
    $zip = Get-ChildItem "$env:USERPROFILE\Downloads" -Filter "DefJam-Android-v18-Menu-CPU*.zip" |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1 -ExpandProperty FullName
    if (!$zip) { throw "Baixe o ZIP v18 em Downloads primeiro." }
    Expand-Archive -LiteralPath $zip -DestinationPath "$env:USERPROFILE\Downloads" -Force
    $env:Path = "C:\dev\gradle-8.7\bin;$env:Path"
    $project = "C:\Users\raylander\Downloads\DefJamFFNY-Android-local-bringup (1)"
    & "$env:USERPROFILE\Downloads\DefJam-Android-v18\Aplicar-v18.ps1" -ProjectRoot $project -DataDir "C:\Games\DefJamFFNY-data" -Variant Release -Build
    & "$env:LOCALAPPDATA\Android\Sdk\platform-tools\adb.exe" install -r "$project\android\app\build\outputs\apk\release\app-release.apk"
    if ($LASTEXITCODE) { throw "ADB nao instalou o APK." }
}
```

O instalador valida todos os hashes antes de escrever e guarda backup em `logs/v18-backups`.
Se detectar fontes diferentes, para antes da gravação. Não desinstale nem limpe os dados.
O build precisa terminar com sucesso antes do ADB. Não foi executado PowerShell/Gradle aqui.

## Teste no celular e log correto

Comece com **guest 2**, já visto abrindo nesta tentativa, **CPUs nativas Auto**, vértices paralelos
ligados e o mesmo driver/async/resolução. Confira Choose Style com A/B e direcional; ao entrar
no tutorial, os controles de luta devem voltar. Depois confira luta, saves e retorno do segundo
plano. Se 2 voltar a travar, use guest 1 e capture a tentativa com o coletor, sem apagar dados.

O HUD mostra `guest efetivo/pedido` e `nativo ≤N`. N é a quantidade de CPUs permitidas ao executor,
não a quantidade de CPUs ocupadas. `[ANDROID-THREAD]` registra papel, função, TID Linux real,
ID lógico do bridge, entrada guest quando conhecida, máscara efetiva e resultado do escalonador.
`[ANDROID-VERTEX-JOBS]` registra quantos lotes usaram 1..8 participantes e helpers criados.
Threads ociosas podem continuar criadas sem estar executando. Use o coletor para tempos por
thread, estado e máscara; PSR sozinho é apenas a última CPU usada.

Compare a mesma luta mudando uma opção por rodada. O teto 30 esconde diferenças somente quando
ambas chegam a 30; não explica uma luta em 18. As opções 120/144/165 continuam sendo limites de
apresentação, sem novo desbloqueio da simulação neste pacote.

Copie o log do telefone imediatamente ao terminar a rodada, antes de abrir outra:

```powershell
$adb = "$env:LOCALAPPDATA\Android\Sdk\platform-tools\adb.exe"
& $adb pull /storage/emulated/0/DefJamFFNY/defjam-native.log "$env:USERPROFILE\Downloads\defjam-v18-guest2-native-auto.log"
if ($LASTEXITCODE) { throw "Nao foi possivel copiar o log do telefone." }
```

Se travar no loading, deixe aberto e use o coletor preservado, sem instalar outro APK antes:

```powershell
& "$env:USERPROFILE\Downloads\DefJam-Android-v18\Coleta-v16\Coletar-Loading-v16.ps1" -ExpectedCores 2
```

O coletor lê primeiro o log do celular. Não use a cópia de `C:\Games` para diagnosticar essa rodada.

## Validação disponível

Seis fixtures de código de produção passaram no host: escolha guest 1..8; recuperação da máscara
nativa Auto/1/2/4/8 sem ampliar guest, incluindo rejeição de afinidade; sequência menu/tutorial e
pacing; snapshot Java com preferências divergentes; PCM/pré-roll/silêncio do callback de áudio;
pool com crescimento, participantes variáveis, falha de worker e saída dos vértices idêntica por bytes.
ASan/UBSan foram usados nos fixtures aplicáveis; LeakSanitizer foi desativado por restrição de procfs
do ambiente. As funções de teste foram executadas diretamente porque pytest não está instalado.
Sintaxe Java e dos arquivos kernel alterados foi verificada; não é compilação Android completa.

Não há SDK/NDK, fontes guest gerados/dados do jogo ou telefone conectado aqui. Não foi gerado APK
nem comprovado ganho de FPS. Abertura/loading/luta/retorno em 1/2/4/8 guest e a utilização efetiva
dos workers v18 continuam pendentes no aparelho. Compilar não substitui essa validação.
