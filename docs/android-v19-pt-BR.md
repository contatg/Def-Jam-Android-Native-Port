# v19 — custo de CPU, interface limpa e build privado no GitHub

Base do patch: **v18**. Versão esperada após compilar: **0.5.5-v19-packets**, versionCode 19.
Este pacote contém fontes e preparação para GitHub. Não há APK pronto nesta etapa.

## Interface solicitada

- O contador mostra somente o FPS real de apresentação.
- Guest, CPUs nativas e teto deixam de aparecer no launcher e no contador.
- O app inicia com **1 guest** e **todas as CPUs nativas permitidas pelo Android**. Preferências
  antigas com guest 2..8 não mudam esse perfil, inclusive na abertura direta da Activity.
- O limite de FPS salvo é preservado. Instalação nova usa 60; esta mudança não baixa o limite.
- A barra inferior mantém START/ENTER. Os botões PAD, MENU e CONTROLES de texto foram removidos.
- A **mesma imagem personalizada da chave** abre o editor no menu e na luta, usando sua posição
  e tamanho salvos. No menu, essa camada desenha só a chave e deixa A/B, direcional e port touch
  receberem os outros toques. A chave permanece acessível mesmo em layouts antigos que a ocultavam.
- O editor continua na mesma Activity, sem destruir a superfície do jogo. Arte, posições e funções
  dos botões de luta, saves, launcher gráfico, driver e fila Vulkan assíncrona foram mantidos.

## Otimizações implementadas

**Busca de critical sections já conhecidas.** Cada thread Android mantém um cache pequeno do
endereço do lock host. A primeira busca continua protegida pelo registro compartilhado; os slots
desse registro nunca são removidos nem realocados. Chamadas seguintes ao mesmo lock evitam
repetir o lock de leitura global para descobrir seu endereço. O mutex recursivo do jogo continua
sendo adquirido/liberado normalmente, com rastreamento de dono e tratamento de contenção.
A inicialização do modo de locks usa uma operação once no Android. Não foi ativado guest lock.

**Índices processados por pacote.** Pacotes não incrementais ARRAY_ELEMENT16/32 do subcanal 0
podem anexar todos os índices disponíveis em uma operação, evitando passar pelo decodificador
geral e pelo contador legado a cada palavra. O parser verifica PUT, alinhamento e a janela de
memória; o executor verifica inicialização, capacidade e modo de diagnóstico. O caminho anterior
continua atendendo pacotes incompatíveis, overflow, outros métodos/subcanais e tracing.
Pacotes parciais retomam do mesmo endereço. Ordem dos índices, contadores, CALL/RETURN/JUMP,
fences e métodos de software foram preservados. Nenhum draw ou objeto foi eliminado.

Essas mudanças reduzem trabalho repetido por lote e por chamada de sincronização. Não exigem
lotes grandes para distribuir vértices. O pool anterior continua usando workers apenas quando
há trabalho suficiente, sem criar ocupação artificial das oito CPUs.

## Os dois logs recebidos

| Rodada v18 | Seu aparelho | Aparelho do amigo, informado como 8 Gen 2 |
| --- | --- | --- |
| GPU registrada | Adreno 610 | Adreno 740 |
| Escala configurada | 1× | 3× |
| Guest aplicado | 2 CPUs | 1 CPU |
| Máscara do render/áudio | CPUs 0..7 | CPUs 0..7 |
| Teto configurado | 30 | 60 |
| Vértices solicitados | 1 participante | 8 participantes |
| Helpers criados | 0 | 0 |
| Trecho final observado | aproximadamente 20–30 FPS | últimas janelas em 59,8 / 59,8 / 58,7 FPS |

No seu log de 6386 linhas, o tutorial/luta tem 18 janelas de apresentação após EnterTutorialMode,
mediana 28 FPS e mínimo 20,2. O último PERF informa 45,21 ms/frame, 5,68 ms de gather e 6,71 ms
de draw-submit, 583 batches/frame. O fim menciona um executor sem movimento por 50 ms; isso
é um indício de espera e não prova sozinho a causa das quedas ou de um travamento persistente.

No log do amigo, de 2665 linhas, Game.StartGame é seguido de renderização 3D e cerca de 59 FPS.
O último PERF informa 16,78 ms/frame, 1,19 ms de gather e 1,07 ms de draw-submit, 359 batches/frame.
Mesmo com 8 participantes pedidos, todos os lotes medidos usaram 1: ficaram abaixo do tamanho
mínimo para os workers. O processo termina perto de 1,83 núcleos equivalentes de CPU.

As cenas, duração e configurações diferem; esses logs não são um benchmark controlado. Não
provam ganho de uma quantidade específica de núcleos, nem throttling. Espera baixa por fence
em async continua sendo espera da CPU, não medição da duração física da GPU.

## Validação desta alteração

Onze fixtures locais de código de produção passaram. Os três novos caminhos exercitam:

- RTL real com mutexes POSIX recursivos e 8 threads concorrentes, cache ligado/desligado,
  colisões, modo single e 32 mil incrementos protegidos. Em 20 mil ciclos após aquecimento,
  as buscas no registro global caem de 40 mil para zero com o cache; os locks reais permanecem.
- Parser real com os handlers de índices de produção: resultado e contadores idênticos nos
  dois caminhos, PUT cortando pacotes em vários pontos, overflow, subcanal diferente, métodos
  incrementais, CALL/RETURN/JUMP, fences e stall. A contabilidade do tradutor foi comparada
  com seus métodos originais completos.
- Handler touch de produção: chave abre o editor nos dois contextos; toques do menu retornam
  para a camada abaixo. Argumentos Java ignoram escolhas CPU antigas e preservam FPS, driver e async.

Os fixtures anteriores de afinidade, pacing/menu, PCM e saída dos vértices também passaram.
ASan/UBSan utilizados onde aplicável; LeakSanitizer desativado por procfs restrito. As funções
foram executadas diretamente porque pytest não está instalado aqui. Sintaxe dos arquivos C
alterados foi verificada nos caminhos Android/Linux; três arquivos Java passaram no parser.

Não há SDK/NDK, fontes guest gerados ou aparelho conectado neste ambiente. Não foi compilado
APK nem medido FPS do v19. Teste de toque/layering Android real, luta longa e retorno do segundo
plano continuam necessários. Os testes host não substituem teste em aparelho.

## Aplicar, compilar e instalar no PC

Baixe o ZIP em Downloads e rode:

```powershell
& {
    $ErrorActionPreference = "Stop"
    Set-ExecutionPolicy -Scope Process Bypass -Force
    $zip = Get-ChildItem "$env:USERPROFILE\Downloads" -Filter "DefJam-Android-v19*.zip" |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1 -ExpandProperty FullName
    if (!$zip) { throw "Baixe o ZIP v19 em Downloads primeiro." }
    Expand-Archive -LiteralPath $zip -DestinationPath "$env:USERPROFILE\Downloads" -Force
    $env:Path = "C:\dev\gradle-8.7\bin;$env:Path"
    $project = "C:\Users\raylander\Downloads\DefJamFFNY-Android-local-bringup (1)"
    & "$env:USERPROFILE\Downloads\DefJam-Android-v19\Aplicar-v19.ps1" -ProjectRoot $project -DataDir "C:\Games\DefJamFFNY-data" -Variant Release -Build
    & "$env:LOCALAPPDATA\Android\Sdk\platform-tools\adb.exe" install -r "$project\android\app\build\outputs\apk\release\app-release.apk"
    if ($LASTEXITCODE) { throw "ADB nao instalou o APK." }
}
```

O instalador valida todos os arquivos antes de alterar qualquer fonte e guarda backup em
logs/v19-backups. O build precisa terminar com sucesso antes do ADB. Não limpa dados nem saves.

## Comparação com v18 no mesmo APK

Com o jogo fechado, o marcador cpu-v18.txt desliga apenas cache de lookup e pacotes novos:

```powershell
$adb = "$env:LOCALAPPDATA\Android\Sdk\platform-tools\adb.exe"
& $adb shell touch /storage/emulated/0/DefJamFFNY/cpu-v18.txt
```

Reabra e confira `CPU fast 0 (v19...)` no log. Para ligar novamente, feche o jogo e remova somente
esse marcador, depois reabra:

```powershell
& $adb shell rm -f /storage/emulated/0/DefJamFFNY/cpu-v18.txt
```

`[ANDROID-PB-FAST]` registra palavras/pacotes realmente processados pelo caminho novo; são
contadores de trabalho, não FPS adicionais. Compare a mesma luta com as mesmas configurações.
Copie cada rodada diretamente do celular antes de abrir a próxima:

```powershell
& $adb pull /storage/emulated/0/DefJamFFNY/defjam-native.log "$env:USERPROFILE\Downloads\defjam-v19-luta.log"
```

## GitHub e APK do PC

O código foi enviado para o repositório privado
[contatg/DefJamFFNY-Android](https://github.com/contatg/DefJamFFNY-Android).
Os [11 testes de controles e runtime passaram no GitHub](https://github.com/contatg/DefJamFFNY-Android/actions/runs/39882940336).

Conforme o pedido mais recente, o APK será o arquivo já compilado no seu PC.
Anexe **app-release.apk** para ele ser enviado à aba Releases. Não é necessário enviar fontes
recompilados ou configurar secrets de compilação. Os dados da versão serão conferidos no APK recebido.
O Actions agora verifica o código; não executa build de APK.

A tentativa anterior de build remoto parou por falta de inputs antes de iniciar SDK/Gradle e não
gerou APK. Isso não altera o APK que você já compilou no PC. O ganho de FPS do v19 ainda depende
de teste em aparelho com essa versão instalada.

Após baixar ou compilar o APK, instale sem apagar saves:

```powershell
& "$env:LOCALAPPDATA\Android\Sdk\platform-tools\adb.exe" install -r "android\app\build\outputs\apk\release\app-release.apk"
```
