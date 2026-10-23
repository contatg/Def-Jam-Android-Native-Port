# v16: travamento no loading com 2 a 8 CPUs — investigação pendente

Não há correção do loading validada nesta etapa. O coletor não modifica o código do jogo,
controles, saves, launcher, limites de FPS, afinidade ou fila Vulkan assíncrona. Para fazer
a coleta não precisa compilar nem instalar outro APK.

## Capturar primeiro a tentativa que trava

1. Conecte o celular ao PC e autorize a depuração USB.
2. Selecione **2 núcleos** no launcher, abra o jogo e aguarde o loading travar.
   Deixe essa tentativa aberta e o jogo em primeiro plano no celular.
3. Extraia o ZIP inteiro em Downloads. No PowerShell do PC:

```powershell
Set-ExecutionPolicy -Scope Process Bypass -Force
& "$env:USERPROFILE\Downloads\DefJam-Android-v17\Coleta-v16\Coletar-Loading-v16.ps1" -ExpectedCores 2
```

Se necessário, acrescente `-Adb "C:\caminho\adb.exe"`. Para vários dispositivos conectados,
use `-DeviceSerial SERIAL`. O caminho padrão é o ADB do Android SDK em LOCALAPPDATA.

O script escreve um **novo ZIP DefJam-Loading-DATA-HORA-IDENTIFICADOR.zip em Downloads**.
Envie esse ZIP. `-ExpectedCores` registra a escolha informada, não muda a configuração do app.
O script não instala, abre, encerra ou reinicia o jogo e não limpa nenhum log ou dado.

Sua primeira operação de coleta é **adb pull** de
`/storage/emulated/0/DefJamFFNY/defjam-native.log`, preservado como `defjam-native-inicio.log`.
Uma segunda cópia, `defjam-native-fim.log`, vem depois das medições. Não usa a cópia de C:\Games.
Registra origem, horário local, metadados remotos e hashes das cópias. A cópia de um log ainda
aberto não é um snapshot atômico: o início é preservado antes de coletar as outras informações.

Também compara `ls -l` e `du -k` das imagens Partition*.img. posix_disk_images em kernel_path.c
cria os tamanhos lógicos do disco Xbox com ftruncate: 2,19 GiB, 500 MiB e três de 750 MiB.
Podem ser arquivos esparsos, com espaço ocupado muito menor. A imagem do gerenciador de
arquivos mostra tamanho lógico; não comprova a alocação física no telefone. O coletor não
trunca, apaga ou reescreve essas imagens, UserData ou TitleData.

`RESUMO.txt` compara a escolha informada com `guest CPU request N` do log. Se selecionar 2 e
o log disser 1, **primeiro investigar a configuração ou se o log pertence à tentativa atual**.
Não interpretar isso como evidência de um travamento com 2 núcleos. A seleção não é confirmada
só pelo parâmetro do coletor; o cabeçalho e a máscara efetiva são evidências diferentes.

O PID vem exclusivamente de `pidof org.defjamffny.android:game`. Se não existir um PID único,
registra a ausência, sem medir o launcher em seu lugar. O shell compara o instante de criação
do processo entre amostras, para não combinar processos após encerramento/reutilização do PID.

As duas amostras registram Linux TID, comm, stat (tempos usuário/kernel), status
(`Cpus_allowed_list`, estado), wchan, uptime antes/depois de cada thread, memória, pressão,
frequências e estado térmico. Tenta `getconf CLK_TCK`; se indisponível, não inventa o valor.
PSR indica o último CPU usado, e não a máscara permitida. Os tempos de cada leitura são
sequenciais; o intervalo real não é simplesmente o sleep de 2 segundos.

Tenta também ler mapas e stacks, capturar estado atual das Activities/janelas, thermalservice,
logcat do PID e preferências via run-as. Android pode negar acesso em builds Release. As falhas
ficam registradas e não são convertidas em conclusões sobre o jogo. Stacks nativas são pedidas
por debuggerd com timeout, sem root; isso pode pausar brevemente o processo **depois** das duas
amostras. Use `-SkipNativeStacks` para omitir essa etapa. O único arquivo enviado ao telefone é
um shell temporário em `/data/local/tmp`, removido ao terminar; não escreve na pasta do jogo.

## Conferência do código atual

- `LauncherActivity.saveUiPreferences()` já usa **commit()**, não apply(). `play()` salva as
  preferências e o INI antes de abrir DefJamActivity. Ainda não envia um snapshot das opções
  por Intent. Se houver divergência confirmada, verificar leitura/cache entre os processos,
  falha de commit e os argumentos, em vez de presumir gravação assíncrona.
- `DefJamActivity.getArguments()` lê guest_cores de SharedPreferences, limita a 1..8 e envia
  `--guest-cores=N`. `src/main.c:android_use_root()` interpreta esse argumento, grava
  RECOMP_GUEST_CORES e emite o cabeçalho. É a cadeia a conferir se o pedido chegar como 1.
- `kernel_bridge.c:xbox_PinToGuestCore()` usa android_guest_cores.h para selecionar N CPUs
  permitidas. É chamada pela entrada guest principal, bridge_thread_main e kernel_timer_thread.
  Isso permite guest simultâneo; não divide automaticamente o trabalho entre N CPUs.
- Foram localizadas inicializações compartilhadas sem once/guard atômico, incluindo
  ke_shadow_init e o início do timer. São **candidatas**, sem evidência de que ocorreram em
  concorrência na tentativa que trava. Não foram alteradas às cegas.
- Há outra inconsistência a investigar: bridge_RtlTryEnterCriticalSection usa uma tabela de
  locks própria, enquanto Enter/Leave usam xbox_cs_shadow em kernel_rtl.c. Ainda falta demonstrar
  que esse ordinal participa do loading deste jogo. Um defeito de código não prova a causa
  deste sintoma. Nenhum lock, fence ou mecanismo de preempção foi removido.
- Não foi ativado RECOMP_GUEST_LOCK. Seu comentário descreve deadlock em loops guest que
  aguardam flags sem chamadas ao kernel. Também não foi rebatizada afinidade guest de 1 CPU
  como execução guest em 8 CPUs, nem fixadas threads sem identificar suas funções.

Linux TID e ID simulado de GetCurrentThreadId são distintos. O v16 usa vários nomes SDLThread
genéricos. Se debuggerd não fornecer símbolos/frames suficientes, será necessária instrumentação
adicional de registro de funções, IDs guest, waits/owners e IRQL, com o código e os símbolos do
APK correspondente. Wchan sozinho identifica uma espera do kernel, não o objeto guest nem seu dono.
O coletor não promete resolver esse mapeamento quando Android negar acesso ou faltar simbolização.

## Evidência disponível e correções das conclusões anteriores

O log enviado antes desta coleta, defjam-native[1](2).log, tem 8424 linhas, request 1, teto 30 e
async Vulkan 1(v16). Chega ao tutorial e à luta. O último D3D está em 6503 e TRANS registra 4678
frames em 6508; depois há cerca de 1900 linhas de kernel sem novos relatórios de apresentação.
Sem eventos de ciclo de vida, isso não distingue pausa/segundo plano de travamento real e não
localiza um loading com 2..8 CPUs. O estado de Activity coletado agora vale para a tentativa atual,
não reconstrói o que aconteceu na rodada antiga.

medida2 captura :game PID16306, cerca de 305% CPU e várias threads ocupadas. Não prova que todo
o custo restante seja uma thread. MemAvailable perto de 3 GiB impede concluir falta de RAM a
partir de MemFree baixo. Frequência e temperatura isoladas não comprovam throttling. Com async,
espera baixa da CPU por fence não mede o tempo físico de execução da GPU. Contagens de chamadas
de kernel não equivalem a tempos gastos nessas chamadas.

## Resultado real desta etapa

| Etapa | Resultado |
| --- | --- |
| Revisão da passagem de configuração e afinidade no fonte v16 | Feita; detalhes acima |
| Coletor shell | Sintaxe e cenários de processo/amostras em fixtures host verificados |
| PowerShell no Windows / permissões Android | Pendente; este ambiente não tem PowerShell nem aparelho/ADB |
| Log novo da tentativa com 2 CPUs | Ainda não obtido do telefone |
| Identificar uma espera e seu produtor/owner durante o loading | Pendente da captura e, se necessário, instrumentação adicional |
| Correção específica do loading | Não aplicada nem declarada pronta |
| Abertura, loading, luta e retorno do segundo plano com 1/2/4/8 CPUs | Não executados aqui |

Depois da captura com 2 CPUs, escolher uma correção específica e comparar uma mudança por vez,
preservando o v16. Se a compatibilidade exigir guest serial, explicar isso na interface e no log,
separando o orçamento das tarefas nativas da execução guest. Para cada 1/2/4/8, registrar abertura,
loading, luta e sair/voltar ao app. Compilação bem-sucedida não substitui esses testes.
Não há ganho de FPS prometido nem alteração de controles/saves/renderização neste pacote.
